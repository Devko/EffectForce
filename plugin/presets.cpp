#include "presets.h"

#include "factory_presets.h"
#include "paths.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <system_error>
#include <unistd.h>

namespace ef {
namespace fs = std::filesystem;

namespace {
constexpr const char* kBuiltin = "builtin:";
}

FileLibrary& presetLibrary() {
    static FileLibrary lib([] {
        FileLibrary::Config c;
        c.exts = {".efp"};
        c.builtinCategory = "Factory";
        for (int i = 0; i < kNumFactoryPresets; ++i) {
            c.builtinNames.push_back(kFactoryPresets[i].name);
            c.builtinCategories.push_back(kFactoryPresets[i].category);
        }
        c.roots = presetRoots;
        c.favFile = "preset_favorites.txt";
        c.recentFile = "preset_recent.txt";
        return c;
    }());
    return lib;
}

bool presetText(const std::string& key, std::string& out) {
    if (key.compare(0, 8, kBuiltin) == 0) {
        const std::string name = key.substr(8);
        for (int i = 0; i < kNumFactoryPresets; ++i)
            if (name == kFactoryPresets[i].name) {
                out = kFactoryPresets[i].text;
                return true;
            }
        return false;
    }
    const std::string path = resolveKey(key, presetRoots());
    return !path.empty() && readFile(path, out);
}

std::string nextUserPreset(std::string* key) {
    const auto roots = presetRoots();
    if (roots.empty()) return {};
    const Root& r = roots.front();
    const std::string dir = r.dir + "/User";
    std::error_code ec;
    fs::create_directories(dir, ec);
    // One past the highest number ever used: the files there, and the folder's own note of the
    // last one saved (.last, hidden from the browser), so a deleted or renamed "User 007" is never
    // reused -- favorites, Recent and projects that named it can't come back as another sound.
    int top = 0;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        int n = 0;
        char tail[8] = {};
        // %8d: a huge number in a file name can't overflow n, or top + 100 below.
        if (std::sscanf(it->path().filename().string().c_str(), "User %8d.ef%1s", &n, tail) == 2 && tail[0] == 'p' && n > 0)
            top = std::max(top, n);
    }
    std::string last;
    if (readFile(dir + "/.last", last, 64)) top = std::max(top, std::clamp(std::atoi(last.c_str()), 0, 99999999));
    for (int n = top + 1; n < top + 100; ++n) {
        char name[32];
        std::snprintf(name, sizeof name, "User %03d.efp", n);
        const std::string path = dir + "/" + name;
        const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);   // claimed: no overwrite
        if (fd < 0) continue;
        ::close(fd);
        writeFileAtomic(dir + "/.last", std::to_string(n) + "\n");
        if (key) *key = r.label + ":User/" + name;
        return path;
    }
    return {};
}

} // namespace ef
