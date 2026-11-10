// core.dll: the mod, loaded and hot-reloaded by portal2-fpv.dll (loader.cpp).
// Everything here runs on the game's main thread, except p2fpv_core_present
// and p2fpv_core_render_release (the render thread).
#include <windows.h>

#include <atomic>

#include "coreapi.h"
#include "crash.h"
#include "devtools.h"
#include "drone.h"
#include "fpvapi.h"
#include "gameplay.h"
#include "log.h"
#include "render.h"
#include "sdk.h"
#include "world.h"

using namespace coreapi;

namespace {

LoaderContext g_ctx{};
bool g_ready = false;
std::atomic<bool> g_in_game{false};

// ClientModeShared: OverrideView (18), CreateMove (24) for the current Portal 2.
constexpr int kOverrideView = 18, kCreateMove = 24;
sdk::VtableHook g_override_view, g_create_move;

using OverrideViewFn = void(__thiscall *)(void *, sdk::CViewSetup *);
using CreateMoveFn = bool(__thiscall *)(void *, float, sdk::CUserCmd *);

void __thiscall override_view(void *self, sdk::CViewSetup *view) {
    g_override_view.orig<OverrideViewFn>()(self, view);
    g_in_game = sdk::in_game();
    static LARGE_INTEGER last{};
    LARGE_INTEGER now, freq;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    float dt = last.QuadPart ? float(now.QuadPart - last.QuadPart) / float(freq.QuadPart) : 0.016f;
    last = now;
    devtools::frame(dt > 0.25f ? 0.25f : dt);
    drone::on_view(view);
}

bool __thiscall create_move(void *self, float sample_time, sdk::CUserCmd *cmd) {
    bool r = g_create_move.orig<CreateMoveFn>()(self, sample_time, cmd);
    drone::on_create_move(cmd);
    return r;
}

// Portal 2 binds F7 to "load quick"; libfpv games launch the drone with it.
// The mod takes it while loaded and gives it back when unloaded.
void take_f7(bool take) {
    if (take) sdk::exec("unbind F7");
    else sdk::exec("bind F7 \"load quick\"");
}

}  // namespace

// Interfaces and hooks; false while the game isn't far enough along (the
// plugin can load before client.dll is ready): retried on server frames.
bool start() {
    if (!sdk::init(g_ctx.engine_factory, g_ctx.server_factory)) return false;
    bool ok = g_override_view.install(sdk::I.client_mode, kOverrideView, reinterpret_cast<void *>(&override_view)) &&
              g_create_move.install(sdk::I.client_mode, kCreateMove, reinterpret_cast<void *>(&create_move));
    fpvlog::write("core: client mode hooks %s", ok ? "installed" : "FAILED");
    if (!ok) {
        g_override_view.restore();
        g_create_move.restore();
        return false;
    }
    drone::load(g_ctx.libfpv_log);
    take_f7(true);
    // after a hot reload libfpv is still loaded and initialised: pick it up
    if (GetModuleHandleA("libfpv.dll") && fpvapi::load(g_ctx.libfpv_log)) {
        gameplay::declare_controls();
        fpvapi::fpv.fpv_overlay_set_action(1, "Move Chell here (Y)");
    }
    g_ready = true;
    fpvlog::write("core: ready");
    return true;
}

extern "C" {

__declspec(dllexport) bool p2fpv_core_load(const LoaderContext *ctx) {
    g_ctx = *ctx;
    crash::install();
    fpvlog::write("core: built %s %s, map '%s'", __DATE__, __TIME__, ctx->map_name ? ctx->map_name : "");
    if (!start()) fpvlog::write("core: the engine isn't ready yet; trying again on the next server frames");
    return true;
}

__declspec(dllexport) void p2fpv_core_unload() {
    if (!g_ready) return;
    drone::unload();
    world::reset();
    if (fpvapi::loaded()) fpvapi::fpv.fpv_world_set_raycast_callback(nullptr, nullptr);  // it points into this DLL
    g_override_view.restore();
    g_create_move.restore();
    take_f7(false);
    g_ready = false;
    fpvlog::write("core: unloaded");
    crash::uninstall();
}

__declspec(dllexport) void p2fpv_core_game_frame(bool) {
    if (!g_ready) {
        static DWORD last = 0;
        static int tries = 0;
        DWORD now = GetTickCount();
        if (tries >= 10 || now - last < 1000) return;
        last = now;
        if (!start() && ++tries == 10) fpvlog::write("core: the engine interfaces never all came up; idle");
        if (!g_ready) return;
    }
    gameplay::server_tick();
    devtools::tick();
}

__declspec(dllexport) void p2fpv_core_level_init(const char *map) {
    if (g_ready) fpvlog::write("core: level %s", map);
}

__declspec(dllexport) void p2fpv_core_server_activate() {
    if (g_ready) gameplay::server_activate();
}

__declspec(dllexport) void p2fpv_core_level_shutdown() {
    if (!g_ready) return;
    drone::level_shutdown();
    gameplay::level_shutdown();
    world::reset();
    g_in_game = false;
}

__declspec(dllexport) void p2fpv_core_present(IDirect3DDevice9 *dev) {
    if (g_ready) render::frame(dev, g_in_game);
}

__declspec(dllexport) void p2fpv_core_device_reset() { render::release(); }

// The loader calls this on the render thread before unloading the core: the
// D3D9 objects are released where they are used.
__declspec(dllexport) void p2fpv_core_render_release() { render::release(); }

}  // extern "C"
