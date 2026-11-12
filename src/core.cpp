// core.dll: the mod, loaded and hot-reloaded by portal2-fpv.dll (loader.cpp).
// Everything here runs on the game's main thread, except p2fpv_core_present
// and p2fpv_core_render_release (the render thread).
#include <atomic>
#include <cstring>

#include "coreapi.h"
#include "crash.h"
#include "devtools.h"
#include "drone.h"
#include "fpvapi.h"
#include "gameplay.h"
#include "kamikaze.h"
#include "log.h"
#include "pilot.h"
#include "platform.h"
#include "render.h"
#include "sdk.h"
#include "world.h"

using namespace coreapi;

namespace {

LoaderContext g_ctx{};
bool g_ready = false;
std::atomic<bool> g_in_game{false};

// ClientModeShared: OverrideView and CreateMove for the current Portal 2
// (SourceAutoRecord; GCC's two destructor slots shift them on Linux).
#if P2FPV_RECOMP
constexpr int kOverrideView = 19, kCreateMove = 25;
#else
constexpr int kOverrideView = 18, kCreateMove = 24;
#endif
sdk::VtableHook g_override_view, g_create_move;

void on_override_view(void *self, sdk::CViewSetup *view) {
    guest::thiscall<void>(g_override_view.original, self, view);
    g_in_game = sdk::in_game();
    static double last = 0;
    double now = platform::seconds();
    float dt = last > 0 ? float(now - last) : 0.016f;
    last = now;
    devtools::frame(dt > 0.25f ? 0.25f : dt);
    drone::on_view(view);
}

bool on_create_move(void *self, float sample_time, sdk::CUserCmd *cmd) {
    bool r = guest::thiscall<bool>(g_create_move.original, self, sample_time, cmd);
    drone::on_create_move(cmd);
    return r;
}

// The functions the game calls in the client mode's vtable.
#if P2FPV_RECOMP
const RtPluginHost *host() { return guest::host; }
void override_view_entry(void *, guest::addr args, void *) {
    on_override_view(guest::h(guest::r32(args)), guest::at<sdk::CViewSetup>(guest::r32(args + 4)));
}
void create_move_entry(void *cpu, guest::addr args, void *) {
    float sample_time;
    std::memcpy(&sample_time, guest::h(args + 4), 4);
    bool r = on_create_move(guest::h(guest::r32(args)), sample_time, guest::at<sdk::CUserCmd>(guest::r32(args + 8)));
    host()->set_return(cpu, r ? 1 : 0);
}
guest::addr override_view_fn() { return host()->define("p2fpv.ClientMode.OverrideView", &override_view_entry, nullptr); }
guest::addr create_move_fn() { return host()->define("p2fpv.ClientMode.CreateMove", &create_move_entry, nullptr); }
#else
void __thiscall override_view_entry(void *self, sdk::CViewSetup *view) { on_override_view(self, view); }
bool __thiscall create_move_entry(void *self, float sample_time, sdk::CUserCmd *cmd) {
    return on_create_move(self, sample_time, cmd);
}
guest::addr override_view_fn() { return guest::g(reinterpret_cast<void *>(&override_view_entry)); }
guest::addr create_move_fn() { return guest::g(reinterpret_cast<void *>(&create_move_entry)); }
#endif

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
    bool ok = g_override_view.install(sdk::I.client_mode, kOverrideView, override_view_fn()) &&
              g_create_move.install(sdk::I.client_mode, kCreateMove, create_move_fn());
    fpvlog::write("core: client mode hooks %s", ok ? "installed" : "FAILED");
    if (!ok) {
        g_override_view.restore();
        g_create_move.restore();
        return false;
    }
    drone::load(g_ctx.libfpv_log);
    take_f7(true);
    // no flight yet: Chell is drawn (an earlier core may have gone with her hidden)
    sdk::exec("ent_fire !player EnableDraw");
    // after a hot reload libfpv is still loaded and initialised: pick it up
    // (or a newer build in its place)
    if (fpvapi::running() && fpvapi::load(g_ctx.libfpv_log)) {
        gameplay::declare_controls();
        pilot::declare();
        kamikaze::declare();
        fpvapi::fpv.fpv_overlay_set_action(1, "Move Chell here (Y)");
    }
    g_ready = true;
    fpvlog::write("core: ready");
    return true;
}

#if defined(_WIN32)
#define CORE_EXPORT __declspec(dllexport)
#else
#define CORE_EXPORT __attribute__((visibility("default")))
#endif

extern "C" {

CORE_EXPORT bool p2fpv_core_load(const LoaderContext *ctx) {
    g_ctx = *ctx;
#if P2FPV_RECOMP
    guest::host = static_cast<const RtPluginHost *>(ctx->host);
#endif
    crash::install();
    fpvlog::write("core: built %s %s, map '%s'", __DATE__, __TIME__, ctx->map_name ? ctx->map_name : "");
    if (!start()) fpvlog::write("core: the engine isn't ready yet; trying again on the next server frames");
    return true;
}

CORE_EXPORT void p2fpv_core_unload() {
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

CORE_EXPORT void p2fpv_core_game_frame(bool) {
    if (!g_ready) {
        static uint32_t last = 0;
        static int tries = 0;
        uint32_t now = platform::ticks_ms();
        if (tries >= 10 || now - last < 1000) return;
        last = now;
        if (!start() && ++tries == 10) fpvlog::write("core: the engine interfaces never all came up; idle");
        if (!g_ready) return;
    }
    gameplay::server_tick();
    devtools::tick();
}

CORE_EXPORT void p2fpv_core_level_init(const char *map) {
    if (g_ready) fpvlog::write("core: level %s", map);
}

CORE_EXPORT void p2fpv_core_server_activate() {
    if (g_ready) gameplay::server_activate();
}

CORE_EXPORT void p2fpv_core_level_shutdown() {
    if (!g_ready) return;
    drone::level_shutdown();
    gameplay::level_shutdown();
    world::reset();
    g_in_game = false;
}

CORE_EXPORT void p2fpv_core_present(IDirect3DDevice9 *dev) {
    if (g_ready) render::frame(dev, g_in_game);
}

CORE_EXPORT void p2fpv_core_device_reset() { render::release(); }

// The loader calls this on the render thread before unloading the core: the
// D3D9 objects are released where they are used.
CORE_EXPORT void p2fpv_core_render_release() { render::release(); }

}  // extern "C"
