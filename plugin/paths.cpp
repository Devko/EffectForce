#include "paths.h"

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace ef {
namespace {

std::vector<Root> roots(const char* env, const char* sub, const char* ssd) {
    std::vector<Root> out;
    if (const char* e = std::getenv(env)) {   // tests: replaces the device folders
        std::string s = e;
        size_t at = 0;
        int n = 0;
        while (at <= s.size()) {
            const size_t colon = s.find(':', at);
            const std::string dir = s.substr(at, colon == std::string::npos ? std::string::npos : colon - at);
            if (!dir.empty()) {
                const char* label = n == 0 ? "plugin" : n == 1 ? "ssd" : nullptr;
                out.push_back({label ? label : "root" + std::to_string(n + 1), dir});
                ++n;
            }
            if (colon == std::string::npos) break;
            at = colon + 1;
        }
        return out;
    }
    const std::string plug = pluginDir();
    if (!plug.empty()) out.push_back({"plugin", plug + "/" + sub});
    out.push_back({"ssd", ssd});
    return out;
}

} // namespace

std::string pluginDir() {
    static const std::string dir = [] {
        FILE* f = std::fopen("/proc/self/maps", "r");
        if (!f) return std::string();
        char line[1024];
        const unsigned long me = reinterpret_cast<unsigned long>(&pluginDir);
        std::string found;
        while (found.empty() && std::fgets(line, sizeof line, f)) {
            unsigned long lo = 0, hi = 0;
            char* p = std::strchr(line, '/');
            if (std::sscanf(line, "%lx-%lx", &lo, &hi) == 2 && me >= lo && me < hi && p) {
                p[std::strcspn(p, "\n")] = 0;
                const char* slash = std::strrchr(p, '/');
                if (slash && slash != p) found.assign(p, static_cast<size_t>(slash - p));
            }
        }
        std::fclose(f);
        return found;
    }();
    return dir;
}

std::vector<Root> presetRoots() { return roots("EF_PRESET_ROOTS", "Presets", "/media/AkaiForce/EffectForce Presets"); }

std::string dataDir() {
    if (const char* e = std::getenv("EF_DATA_DIR")) return e;
    return pluginDir();
}

std::string resolveKey(const std::string& key, const std::vector<Root>& rs) {
    const size_t colon = key.find(':');
    if (colon == std::string::npos) return {};
    const std::string label = key.substr(0, colon), rel = key.substr(colon + 1);
    // Keys come from saved state text: never a way out of the root ("Lead...efp" is fine).
    if (rel.empty() || rel[0] == '/') return {};
    for (const char c : rel)   // a NUL would cut the path short ("x/..\0" = "x/.."), a newline breaks lists
        if (static_cast<unsigned char>(c) < 0x20) return {};
    for (size_t at = 0; at <= rel.size();) {
        const size_t slash = std::min(rel.find('/', at), rel.size());
        const std::string part = rel.substr(at, slash - at);
        if (part == ".." || part == ".") return {};
        at = slash + 1;
    }
    for (const Root& r : rs)
        if (r.label == label) return r.dir + "/" + rel;
    return {};
}

bool writeFileAtomic(const std::string& path, const std::string& text, bool durable) {
    // A temp name of our own (two instances may save the same list at once), written in full
    // and synced before it replaces the file; removed on any failure.
    static std::atomic<unsigned> counter{0};
    const std::string tmp = path + ".new." + std::to_string(getpid()) + "." + std::to_string(counter.fetch_add(1));
    const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    if (fd < 0) return false;
    bool ok = true;
    for (size_t done = 0; ok && done < text.size();) {
        const ssize_t w = ::write(fd, text.data() + done, text.size() - done);
        if (w < 0 && errno == EINTR) continue;
        ok = w > 0;
        if (ok) done += static_cast<size_t>(w);
    }
    ok = ok && (!durable || ::fsync(fd) == 0);
    ok = ::close(fd) == 0 && ok;
    ok = ok && std::rename(tmp.c_str(), path.c_str()) == 0;
    if (!ok) {
        ::unlink(tmp.c_str());
        return false;
    }
    if (durable) {   // the rename itself: sync the folder that holds it
        const size_t slash = path.find_last_of('/');
        const std::string dir = slash == std::string::npos ? "." : (slash == 0 ? "/" : path.substr(0, slash));
        const int dfd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (dfd >= 0) {
            ::fsync(dfd);
            ::close(dfd);
        }
    }
    return true;
}

bool readFile(const std::string& path, std::string& out, size_t maxBytes) {
    struct stat st{};
    if (::stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) return false;
    if (static_cast<unsigned long long>(st.st_size) > maxBytes) return false;   // not one of ours: don't read it whole
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::string s(static_cast<size_t>(st.st_size), '\0');
    const size_t got = s.empty() ? 0 : std::fread(&s[0], 1, s.size(), f);
    std::fclose(f);
    s.resize(got);
    out = std::move(s);
    return true;
}

} // namespace ef
