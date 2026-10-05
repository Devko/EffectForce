#include "library.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <sstream>
#include <system_error>

namespace ef {
namespace fs = std::filesystem;

namespace {

constexpr int kMaxDepth = 4;   // category / subfolders: deep enough, never a runaway walk

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool lessNoCase(const std::string& a, const std::string& b) {
    const std::string la = lower(a), lb = lower(b);
    return la != lb ? la < lb : a < b;
}

std::string stemOf(const std::string& file) {
    const size_t slash = file.find_last_of('/');
    std::string s = slash == std::string::npos ? file : file.substr(slash + 1);
    const size_t dot = s.rfind('.');
    return dot == std::string::npos || dot == 0 ? s : s.substr(0, dot);
}

// Longest prefix ending in " - " that every name shares ("" if none).
std::string sharedPrefix(const std::vector<std::string>& names) {
    if (names.empty()) return {};
    std::string p = names.front();
    for (const std::string& n : names) {
        size_t i = 0;
        while (i < p.size() && i < n.size() && p[i] == n[i]) ++i;
        p.resize(i);
    }
    const size_t dash = p.rfind(" - ");
    if (dash == std::string::npos) return {};
    p.resize(dash + 3);
    for (const std::string& n : names)
        if (n.size() <= p.size()) return {};   // never strip a name down to nothing
    return p;
}

struct Found {
    std::string key, path, category, stem;
};

void walk(const Root& root, const std::vector<std::string>& exts, std::vector<Found>& out) {
    std::error_code ec;
    if (!fs::is_directory(root.dir, ec)) return;
    fs::recursive_directory_iterator it(root.dir, fs::directory_options::skip_permission_denied, ec), end;
    for (; !ec && it != end; it.increment(ec)) {
        if (it.depth() >= kMaxDepth) {
            it.disable_recursion_pending();
            continue;
        }
        const fs::directory_entry& e = *it;
        std::error_code fe;   // one entry's trouble skips that entry; `ec` is the walk's own
        if (e.is_symlink(fe)) {   // never follow links: no loops, no surprises
            if (e.is_directory(fe)) it.disable_recursion_pending();
            continue;
        }
        if (!e.is_regular_file(fe)) continue;
        const std::string ext = lower(e.path().extension().string());
        if (std::find(exts.begin(), exts.end(), ext) == exts.end()) continue;
        const std::string rel = fs::relative(e.path(), root.dir, fe).generic_string();
        if (fe || rel.empty()) continue;
        if (rel[0] == '.' || rel.find("/.") != std::string::npos) continue;   // hidden files, "._" macOS junk
        // A control character (a newline) would break the one-key-per-line lists and state
        // text, and resolveKey refuses such keys anyway.
        if (std::any_of(rel.begin(), rel.end(), [](char c) { return static_cast<unsigned char>(c) < 0x20; })) continue;
        const size_t slash = rel.find('/');
        out.push_back({root.label + ":" + rel, e.path().string(),
                       slash == std::string::npos ? "Unsorted" : rel.substr(0, slash), stemOf(rel)});
    }
}

} // namespace

int Listing::categoryOf(int item) const {
    if (item < 0 || item >= static_cast<int>(items.size())) return -1;
    for (size_t c = 0; c < categories.size(); ++c)
        if (categories[c] == items[static_cast<size_t>(item)].category) return static_cast<int>(c);
    return -1;
}

std::string Listing::label(const std::string& key) const {
    const int i = find(key);
    if (i >= 0) {
        const Entry& e = items[static_cast<size_t>(i)];
        return e.category + " / " + e.name;
    }
    const size_t colon = key.find(':');
    return stemOf(colon == std::string::npos ? key : key.substr(colon + 1));
}

std::shared_ptr<const Listing> scanLibrary(const FileLibrary::Config& cfg) {
    auto L = std::make_shared<Listing>();
    std::vector<Found> files;
    for (const Root& r : cfg.roots()) walk(r, cfg.exts, files);

    std::map<std::string, std::vector<Found>, bool (*)(const std::string&, const std::string&)> byCat(lessNoCase);
    for (Found& f : files) byCat[f.category].push_back(std::move(f));

    auto addCategory = [&L](const std::string& name, std::vector<Entry> entries) {
        if (entries.empty()) return;
        std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
            return a.name != b.name ? lessNoCase(a.name, b.name) : a.key < b.key;
        });
        L->categories.push_back(name);
        L->members.emplace_back();
        for (Entry& e : entries) {
            const int idx = static_cast<int>(L->items.size());
            L->byKey[e.key] = idx;
            L->members.back().push_back(idx);
            L->items.push_back(std::move(e));
        }
    };

    // Keep the built-ins in their own order (Classic first), not A -> Z, grouped by category in
    // the order the categories first appear.
    std::vector<std::string> builtinCats;
    std::vector<std::vector<Entry>> builtins;
    for (size_t i = 0; i < cfg.builtinNames.size(); ++i) {
        const std::string& cat = i < cfg.builtinCategories.size() ? cfg.builtinCategories[i] : cfg.builtinCategory;
        size_t c = 0;
        while (c < builtinCats.size() && builtinCats[c] != cat) ++c;
        if (c == builtinCats.size()) {
            builtinCats.push_back(cat);
            builtins.emplace_back();
        }
        builtins[c].push_back({"builtin:" + cfg.builtinNames[i], "", cat, cfg.builtinNames[i], static_cast<int>(i)});
    }
    for (size_t c = 0; c < builtinCats.size(); ++c) {
        L->categories.push_back(builtinCats[c]);
        L->members.emplace_back();
        for (Entry& e : builtins[c]) {
            const int idx = static_cast<int>(L->items.size());
            L->byKey[e.key] = idx;
            L->members.back().push_back(idx);
            L->items.push_back(std::move(e));
        }
    }

    for (auto& [cat, found] : byCat) {
        std::vector<std::string> stems;
        for (const Found& f : found) stems.push_back(f.stem);
        const std::string prefix = sharedPrefix(stems);
        const bool taken = std::find(builtinCats.begin(), builtinCats.end(), cat) != builtinCats.end();
        const std::string shown = taken ? cat + " (files)" : cat;   // never a built-in category
        std::vector<Entry> entries;
        for (Found& f : found)
            entries.push_back({f.key, f.path, shown, f.stem.substr(prefix.size()), -1});
        addCategory(shown, std::move(entries));
    }
    return L;
}

FileLibrary::FileLibrary(Config cfg) : cfg_(std::move(cfg)) {}

std::shared_ptr<const Listing> FileLibrary::listing() const {
    std::lock_guard<std::mutex> lk(mtx_);
    if (!listing_) listing_ = scanLibrary(cfg_);
    return listing_;
}

void FileLibrary::rescan() {
    auto fresh = scanLibrary(cfg_);   // outside the lock: a scan touches the disk
    std::lock_guard<std::mutex> lk(mtx_);
    listing_ = std::move(fresh);
}

void FileLibrary::loadLists() const {
    if (listsLoaded_) return;
    listsLoaded_ = true;
    const std::string dir = dataDir();
    if (dir.empty()) return;
    std::string text;
    if (!cfg_.favFile.empty() && readFile(dir + "/" + cfg_.favFile, text)) {
        std::istringstream s(text);
        for (std::string line; std::getline(s, line);) {
            if (!line.empty() && line.back() == '\r') line.pop_back();   // edited on Windows
            if (!line.empty()) fav_.insert(line);
        }
    }
    if (!cfg_.recentFile.empty() && readFile(dir + "/" + cfg_.recentFile, text)) {
        std::istringstream s(text);
        for (std::string line; std::getline(s, line);) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (!line.empty() && recent_.size() < cfg_.recentMax) recent_.push_back(line);
        }
    }
}

// Saves run outside mtx_ (they touch the disk), one at a time, and an older list never
// overwrites a newer one (two instances can touch Recent at once).
void FileLibrary::saveList(const std::string& file, const std::vector<std::string>& keys, uint64_t gen) const {
    const std::string dir = dataDir();
    if (dir.empty() || file.empty()) return;
    std::lock_guard<std::mutex> lk(saveMtx_);
    uint64_t& last = file == cfg_.favFile ? savedFav_ : savedRecent_;
    if (gen <= last) return;
    last = gen;
    std::string text;
    for (const std::string& k : keys) text += k + "\n";
    writeFileAtomic(dir + "/" + file, text, false);   // a list: losing the last change to a power cut is fine
}

bool FileLibrary::isFavorite(const std::string& key) const {
    std::lock_guard<std::mutex> lk(mtx_);
    loadLists();
    return fav_.count(key) > 0;
}

void FileLibrary::setFavorite(const std::string& key, bool on) {
    std::vector<std::string> keys;
    uint64_t gen = 0;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        loadLists();
        if (on == (fav_.count(key) > 0)) return;
        if (on) fav_.insert(key);
        else fav_.erase(key);
        keys.assign(fav_.begin(), fav_.end());
        gen = ++gen_;
    }
    saveList(cfg_.favFile, keys, gen);
}

std::vector<std::string> FileLibrary::favorites() const {
    std::set<std::string> keys;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        loadLists();
        keys = fav_;
    }
    const auto L = listing();
    std::vector<std::string> out;
    for (const Entry& e : L->items)
        if (keys.count(e.key)) out.push_back(e.key);
    return out;
}

void FileLibrary::touchRecent(const std::string& key) {
    std::vector<std::string> keys;
    uint64_t gen = 0;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        loadLists();
        if (!recent_.empty() && recent_.front() == key) return;
        recent_.erase(std::remove(recent_.begin(), recent_.end(), key), recent_.end());
        recent_.insert(recent_.begin(), key);
        if (recent_.size() > cfg_.recentMax) recent_.resize(cfg_.recentMax);
        keys = recent_;
        gen = ++gen_;
    }
    saveList(cfg_.recentFile, keys, gen);
}

std::vector<std::string> FileLibrary::recent() const {
    std::vector<std::string> keys;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        loadLists();
        keys = recent_;
    }
    const auto L = listing();
    std::vector<std::string> out;
    for (const std::string& k : keys)
        if (L->find(k) >= 0) out.push_back(k);
    return out;
}

} // namespace ef
