#include "fpvapi.h"

#include <windows.h>

#include <string>

#include "crash.h"
#include "log.h"
#include "paths.h"

namespace fpvapi {

Api fpv;

namespace {
bool g_loaded = false;
bool g_failed = false;
}  // namespace

bool loaded() { return g_loaded; }

bool load(fpv_log_fn log) {
    if (g_loaded) return true;
    if (g_failed) return false;
    g_failed = true;  // until proven otherwise: don't retry on every key press

    std::string dll = paths::mod_dir() + "\\libfpv.dll";
    // libfpv pins itself (fpv_global_init) and survives core reloads: a reload
    // finds it loaded and initialised already.
    HMODULE lib = GetModuleHandleA(dll.c_str());
    crash::note("LoadLibrary libfpv");
    if (!lib) lib = LoadLibraryA(dll.c_str());
    if (!lib) {
        fpvlog::write("cannot load %s (error %lu)", dll.c_str(), GetLastError());
        return false;
    }
    bool ok = true;
#define FPV_FN(name)                                                                                     \
    fpv.name = reinterpret_cast<decltype(fpv.name)>(reinterpret_cast<void *>(GetProcAddress(lib, #name))); \
    if (!fpv.name) {                                                                                     \
        fpvlog::write("libfpv.dll has no %s (an older libfpv? ship the one built with the mod)", #name);  \
        ok = false;                                                                                      \
    }
    FPV_FUNCTIONS(FPV_FN)
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
    // Chell stays where she is: test chambers are small, nothing streams
    // out, so no stand-in; and she has no animation holding a radio.
    crash::note("fpv_pilot_set_support");
    fpv.fpv_pilot_set_support(0, 0, 0);
    g_failed = false;
    g_loaded = true;
    fpvlog::write("libfpv %s initialised, data in %s", fpv.fpv_version_string(), data_dir.c_str());
    return true;
}

}  // namespace fpvapi
