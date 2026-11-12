// The contract between the loader (portal2-fpv.dll, the server plugin that
// stays loaded) and the core (core.dll, hot-reloaded on every new build).
#pragma once

#include <cstdint>

#include <libfpv.h>

#include "guest.h"

struct IDirect3DDevice9;

namespace coreapi {

struct LoaderContext {
    uint32_t size;
    guest::addr engine_factory;  // from IServerPluginCallbacks::Load (game functions)
    guest::addr server_factory;
    // macOS: source-recomp's host table (rt_plugin.h), nullptr on Windows
    const void *host;
    // libfpv keeps its log callback for the life of the process: it lives in
    // the loader, which is never unloaded (docs/INTEGRATION.md#building-and-loading).
    fpv_log_fn libfpv_log;
    bool in_level;         // between LevelInit and LevelShutdown
    const char *map_name;  // the current map, "" outside a level
};

// Exported by the core (extern "C"), called by the loader:
//   bool p2fpv_core_load(const LoaderContext *)  - after loading (also mid-level, after a reload)
//   void p2fpv_core_unload()                    - before FreeLibrary: remove every hook, stop threads
//   void p2fpv_core_game_frame(bool simulating) - IServerPluginCallbacks::GameFrame, main thread
//   void p2fpv_core_level_init(const char *map)
//   void p2fpv_core_server_activate()           - the map's entities exist, precaching is still allowed
//   void p2fpv_core_level_shutdown()
//   void p2fpv_core_present(IDirect3DDevice9 *) - the render thread, right before Present
//                                                (macOS: the native Metal device)
//   void p2fpv_core_device_reset()              - the render thread, before IDirect3DDevice9::Reset
using LoadFn = bool (*)(const LoaderContext *);
using VoidFn = void (*)();
using GameFrameFn = void (*)(bool);
using LevelInitFn = void (*)(const char *);
using PresentFn = void (*)(IDirect3DDevice9 *);

}  // namespace coreapi
