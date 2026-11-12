#include "fpvapi.h"

#include <cctype>
#include <string>
#include <vector>

#include "crash.h"
#include "log.h"
#include "paths.h"
#include "platform.h"

#if defined(_WIN32)
#include <windows.h>
#include <psapi.h>
#else
#include <dirent.h>
#include <dlfcn.h>
#include <mach-o/dyld.h>
#endif

namespace fpvapi {

Api fpv;

namespace {
bool g_loaded = false;
bool g_failed = false;

// --- which libfpv --------------------------------------------------------------------------
// Development builds go to libfpv-live/ while the game runs (tools/deploy.sh):
// a loaded libfpv is locked and pinned, so it can't be replaced in place. The
// newest of libfpv.dll (.dylib) and those is the one to run. On a core load
// (also a hot reload) a newer one replaces the running one: that is
// terminated (its threads and the Configurator server stop), stays mapped
// (pinned), and the new copy starts fresh. Each copy has its own globals,
// Betaflight included.

#if defined(_WIN32)
using Lib = HMODULE;
constexpr const char *kExt = ".dll";
void *symbol(Lib lib, const char *name) { return reinterpret_cast<void *>(GetProcAddress(lib, name)); }
Lib open_lib(const std::string &path) { return LoadLibraryA(path.c_str()); }
std::string open_error() { return "error " + std::to_string(GetLastError()); }
#else
using Lib = void *;
constexpr const char *kExt = ".dylib";
void *symbol(Lib lib, const char *name) { return dlsym(lib, name); }
Lib open_lib(const std::string &path) { return dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL); }
std::string open_error() {
    const char *e = dlerror();
    return e ? e : "unknown error";
}
#endif

std::string lower(std::string s) {
    for (char &c : s) c = char(tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string base_name(const std::string &path) { return path.substr(path.find_last_of("\\/") + 1); }

// The dev copies in libfpv-live/ (libfpv-*.dll / .dylib).
std::vector<std::string> live_builds() {
    std::vector<std::string> out;
    std::string dir = paths::mod_dir() + "/libfpv-live/";
#if defined(_WIN32)
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((dir + "libfpv-*.dll").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do out.push_back(dir + fd.cFileName);
    while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    if (DIR *d = opendir(dir.c_str())) {
        while (dirent *e = readdir(d)) {
            std::string n = e->d_name;
            if (n.rfind("libfpv-", 0) == 0 && n.size() > 6 && n.substr(n.size() - 6) == ".dylib") out.push_back(dir + n);
        }
        closedir(d);
    }
#endif
    return out;
}

// The newest libfpv build on disk.
std::string newest_build() {
    std::string best = paths::mod_dir() + "/libfpv" + kExt;
    uint64_t best_time = platform::file_stamp(best);
    for (const std::string &p : live_builds()) {
        uint64_t t = platform::file_stamp(p);
        if (t > best_time) best_time = t, best = p;
    }
    return best;
}

// The libfpv running in this process (loaded and initialised), or nullptr.
Lib running_libfpv(std::string *path) {
    auto initialised = [](Lib lib) {
        auto is_init = reinterpret_cast<int (*)()>(symbol(lib, "fpv_is_initialized"));
        return is_init && is_init();
    };
#if defined(_WIN32)
    HMODULE mods[1024];
    DWORD bytes = 0;
    if (!EnumProcessModules(GetCurrentProcess(), mods, sizeof mods, &bytes)) return nullptr;
    for (DWORD i = 0; i < bytes / sizeof(HMODULE); i++) {
        char name[MAX_PATH];
        if (!GetModuleFileNameA(mods[i], name, sizeof name)) continue;
        if (lower(base_name(name)).rfind("libfpv", 0) != 0) continue;
        if (initialised(mods[i])) {
            if (path) *path = name;
            return mods[i];
        }
    }
#else
    for (uint32_t i = 0, n = _dyld_image_count(); i < n; ++i) {
        const char *name = _dyld_get_image_name(i);
        if (!name || base_name(name).rfind("libfpv", 0) != 0) continue;
        Lib lib = dlopen(name, RTLD_NOW | RTLD_NOLOAD);
        if (lib && initialised(lib)) {
            if (path) *path = name;
            return lib;
        }
    }
#endif
    return nullptr;
}

// Old dev copies nobody runs any more (on Windows a loaded one can't be deleted: skipped).
void clean_live_dir(const std::string &keep) {
    std::string running;
    running_libfpv(&running);
    for (const std::string &p : live_builds())
        if (lower(base_name(p)) != lower(base_name(keep)) && lower(base_name(p)) != lower(base_name(running)))
            platform::remove_file(p);
}

}  // namespace

bool loaded() { return g_loaded; }
bool running() { return running_libfpv(nullptr) != nullptr; }

bool load(fpv_log_fn log) {
    if (g_loaded) return true;
    if (g_failed) return false;
    g_failed = true;  // until proven otherwise: don't retry on every key press

    std::string dll = newest_build(), running_path;
    Lib lib = running_libfpv(&running_path);
    if (lib && lower(running_path) != lower(dll)) {
        // a newer build: stop the running one for good, start the new one
        fpvlog::write("libfpv: %s replaces the running %s", dll.c_str(), running_path.c_str());
        crash::note("fpv_global_terminate (old libfpv)");
        auto terminate = reinterpret_cast<void (*)()>(symbol(lib, "fpv_global_terminate"));
        if (terminate) terminate();
        lib = nullptr;
    }
    crash::note("LoadLibrary libfpv");
    if (!lib) lib = open_lib(dll);
    if (!lib) {
        fpvlog::write("cannot load %s (%s)", dll.c_str(), open_error().c_str());
        return false;
    }
    clean_live_dir(dll);
    bool ok = true;
#define FPV_FN(name)                                                                                     \
    fpv.name = reinterpret_cast<decltype(fpv.name)>(symbol(lib, #name));                                 \
    if (!fpv.name) {                                                                                     \
        fpvlog::write("libfpv has no %s (an older libfpv? ship the one built with the mod)", #name);      \
        ok = false;                                                                                      \
    }
    FPV_FUNCTIONS(FPV_FN)
    FPV_KAMIKAZE_FUNCTIONS(FPV_FN)
#undef FPV_FN
    if (!ok) return false;

    // the pilot's radio mappings, tune and settings, shared by every libfpv game
    char shared[1024];
    std::string data_dir;
    if (fpv.fpv_shared_data_dir(shared, sizeof shared) == FPV_OK) data_dir = shared;
    else data_dir = paths::data_dir();
    fpv_global_config cfg;
    fpv.fpv_global_config_defaults(&cfg);
    cfg.data_dir = data_dir.c_str();
    // Source: X forward, Z up, Y left (so right is -Y); 1 unit = 1 inch
    cfg.coords = {FPV_AXIS_NEG_Y, FPV_AXIS_POS_Z, FPV_AXIS_POS_X, kUnitsPerMeter};
    cfg.log = log;
    cfg.log_level = FPV_LOG_INFO;
    crash::note("fpv_global_init");
    fpvlog::write("libfpv: initialising, data in %s", data_dir.c_str());
    fpv_result r = fpv.fpv_global_init(&cfg);
    if (r != FPV_OK && r != FPV_ERROR_ALREADY_INITIALIZED) {
        fpvlog::write("fpv_global_init: %s (%s)", fpv.fpv_result_string(r), fpv.fpv_last_error());
        return false;
    }
    g_failed = false;
    g_loaded = true;
    fpvlog::write("libfpv %s initialised, data in %s", fpv.fpv_version_string(), data_dir.c_str());
    return true;
}

}  // namespace fpvapi
