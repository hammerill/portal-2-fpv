#include "fpvapi.h"

#include <windows.h>
#include <psapi.h>

#include <cctype>
#include <string>

#include "crash.h"
#include "log.h"
#include "paths.h"

namespace fpvapi {

Api fpv;

namespace {
bool g_loaded = false;
bool g_failed = false;

// --- which libfpv --------------------------------------------------------------------------
// Development builds go to libfpv-live\ while the game runs (tools/deploy.sh):
// a loaded libfpv is locked and pinned, so it can't be replaced in place. The
// newest of libfpv.dll and those is the one to run. On a core load (also a hot
// reload) a newer one replaces the running one: that is terminated (its threads
// and the Configurator server stop), stays mapped (pinned), and the new copy
// starts fresh. Each copy has its own globals, Betaflight included.

std::string lower(std::string s) {
    for (char &c : s) c = char(tolower(static_cast<unsigned char>(c)));
    return s;
}

bool newer(const FILETIME &a, const FILETIME &b) { return CompareFileTime(&a, &b) > 0; }

// The newest libfpv build on disk.
std::string newest_build() {
    std::string best = paths::mod_dir() + "\\libfpv.dll";
    FILETIME best_time{};
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (GetFileAttributesExA(best.c_str(), GetFileExInfoStandard, &a)) best_time = a.ftLastWriteTime;
    std::string dir = paths::mod_dir() + "\\libfpv-live\\";
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((dir + "libfpv-*.dll").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (newer(fd.ftLastWriteTime, best_time)) best_time = fd.ftLastWriteTime, best = dir + fd.cFileName;
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    return best;
}

// The libfpv running in this process (loaded and initialised), or nullptr.
HMODULE running_libfpv(std::string *path) {
    HMODULE mods[1024];
    DWORD bytes = 0;
    if (!EnumProcessModules(GetCurrentProcess(), mods, sizeof mods, &bytes)) return nullptr;
    for (DWORD i = 0; i < bytes / sizeof(HMODULE); i++) {
        char name[MAX_PATH];
        if (!GetModuleFileNameA(mods[i], name, sizeof name)) continue;
        std::string base = lower(name);
        base = base.substr(base.find_last_of("\\/") + 1);
        if (base.rfind("libfpv", 0) != 0) continue;
        auto is_init = reinterpret_cast<int (*)()>(reinterpret_cast<void *>(GetProcAddress(mods[i], "fpv_is_initialized")));
        if (is_init && is_init()) {
            if (path) *path = name;
            return mods[i];
        }
    }
    return nullptr;
}

// Old dev copies nobody runs any more (a loaded one can't be deleted: skipped).
void clean_live_dir(const std::string &keep) {
    std::string dir = paths::mod_dir() + "\\libfpv-live\\";
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((dir + "libfpv-*.dll").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        std::string p = dir + fd.cFileName;
        if (lower(p) != lower(keep)) DeleteFileA(p.c_str());
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

}  // namespace

bool loaded() { return g_loaded; }
bool running() { return running_libfpv(nullptr) != nullptr; }

bool load(fpv_log_fn log) {
    if (g_loaded) return true;
    if (g_failed) return false;
    g_failed = true;  // until proven otherwise: don't retry on every key press

    std::string dll = newest_build(), running_path;
    HMODULE lib = running_libfpv(&running_path);
    if (lib && lower(running_path) != lower(dll)) {
        // a newer build: stop the running one for good, start the new one
        fpvlog::write("libfpv: %s replaces the running %s", dll.c_str(), running_path.c_str());
        crash::note("fpv_global_terminate (old libfpv)");
        auto terminate = reinterpret_cast<void (*)()>(reinterpret_cast<void *>(GetProcAddress(lib, "fpv_global_terminate")));
        if (terminate) terminate();
        lib = nullptr;
    }
    crash::note("LoadLibrary libfpv");
    if (!lib) lib = LoadLibraryA(dll.c_str());
    if (!lib) {
        fpvlog::write("cannot load %s (error %lu)", dll.c_str(), GetLastError());
        return false;
    }
    clean_live_dir(dll);
    bool ok = true;
#define FPV_FN(name)                                                                                     \
    fpv.name = reinterpret_cast<decltype(fpv.name)>(reinterpret_cast<void *>(GetProcAddress(lib, #name))); \
    if (!fpv.name) {                                                                                     \
        fpvlog::write("libfpv.dll has no %s (an older libfpv? ship the one built with the mod)", #name);  \
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
