// portal2-fpv.so on macOS: an arm64 dylib (despite the name) that Portal 2,
// recompiled by source-recomp, loads as its server plugin
// (portal2/addons/portal2-fpv.vdf). source-recomp sees a Mach-O image where
// the game asks for a Linux module and loads it natively (rt_plugin.h): the
// game calls the functions defined here through its own vtables.
//
// The same thin shell as loader.cpp on Windows:
// - the log, and libfpv's log callback (libfpv keeps it until the game exits);
// - the IServerPluginCallbacks the engine calls, forwarded to the core;
// - hot reload: a new core.dylib is copied to core-live/ and swapped in at the
//   next check, mid-level. An old core stays mapped (its render objects are
//   released by it on the next frame, and nothing calls it any more);
// - the Present / Reset hooks of the Metal renderer, forwarded to the core
//   under a lock, so a reload never pulls the core out from under them;
// - the developer bridge: lines written to fpv-data/cmd.txt run as console
//   commands, and the console is logged to fpv-data/console.log (in the
//   game's user folder: source-recomp keeps what the game writes there).
#include <dlfcn.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <shared_mutex>
#include <string>

#include "coreapi.h"
#include "guest.h"
#include "log.h"
#include "paths.h"
#include "platform.h"
#include "rt_plugin.h"

using namespace coreapi;

namespace {

RtPlugin *g_plugin = nullptr;
LoaderContext g_ctx{};
std::string g_map;

// --- the core ---------------------------------------------------------------------

void *g_core = nullptr;
uint64_t g_core_stamp = 0;
int g_core_serial = 0;

LoadFn g_core_load = nullptr;
VoidFn g_core_unload = nullptr;
GameFrameFn g_core_game_frame = nullptr;
LevelInitFn g_core_level_init = nullptr;
VoidFn g_core_server_activate = nullptr;
VoidFn g_core_level_shutdown = nullptr;
PresentFn g_core_present = nullptr;          // guarded by g_render_lock
VoidFn g_core_device_reset = nullptr;        // same
VoidFn g_release_pending = nullptr;          // same: an unloaded core's render_release, for the next frame
std::shared_mutex g_render_lock;

std::string core_path() { return paths::mod_dir() + "/core.dylib"; }

bool core_changed() {
    uint64_t stamp = platform::file_stamp(core_path());
    if (!stamp || stamp == g_core_stamp) return false;
    g_core_stamp = stamp;
    return true;
}

void unload_core() {
    if (!g_core) return;
    {
        std::unique_lock<std::shared_mutex> lock(g_render_lock);  // waits for a Present in progress
        g_release_pending = reinterpret_cast<VoidFn>(dlsym(g_core, "p2fpv_core_render_release"));
        g_core_present = nullptr;
        g_core_device_reset = nullptr;
    }
    if (g_core_unload) g_core_unload();
    // not dlclose'd: the render thread still calls its render_release once
    g_core = nullptr;
    g_core_load = nullptr;
    g_core_unload = g_core_server_activate = g_core_level_shutdown = nullptr;
    g_core_game_frame = nullptr;
    g_core_level_init = nullptr;
    fpvlog::write("loader: core unloaded");
}

bool copy_file(const std::string &from, const std::string &to) {
    FILE *in = std::fopen(from.c_str(), "rb");
    if (!in) return false;
    FILE *out = std::fopen(to.c_str(), "wb");
    if (!out) {
        std::fclose(in);
        return false;
    }
    char buf[65536];
    size_t n;
    bool ok = true;
    while ((n = std::fread(buf, 1, sizeof buf, in)) > 0) ok = ok && std::fwrite(buf, 1, n, out) == n;
    std::fclose(in);
    return std::fclose(out) == 0 && ok;
}

void load_core() {
    unload_core();
    // a fresh name each time: dyld would hand back the copy it already has
    std::string dir = paths::mod_dir() + "/core-live";
    platform::make_dir(dir);
    std::string live = dir + "/core-" + std::to_string(getpid()) + "-" + std::to_string(++g_core_serial) + ".dylib";
    if (!copy_file(core_path(), live)) {
        fpvlog::write("loader: cannot copy the core to %s", live.c_str());
        return;
    }
    void *m = dlopen(live.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!m) {
        fpvlog::write("loader: cannot load %s: %s", live.c_str(), dlerror());
        return;
    }
    auto get = [m](const char *name) { return dlsym(m, name); };
    auto load = reinterpret_cast<LoadFn>(get("p2fpv_core_load"));
    if (!load) {
        fpvlog::write("loader: the core exports no p2fpv_core_load");
        return;
    }
    g_core = m;
    g_core_load = load;
    g_core_unload = reinterpret_cast<VoidFn>(get("p2fpv_core_unload"));
    g_core_game_frame = reinterpret_cast<GameFrameFn>(get("p2fpv_core_game_frame"));
    g_core_level_init = reinterpret_cast<LevelInitFn>(get("p2fpv_core_level_init"));
    g_core_server_activate = reinterpret_cast<VoidFn>(get("p2fpv_core_server_activate"));
    g_core_level_shutdown = reinterpret_cast<VoidFn>(get("p2fpv_core_level_shutdown"));
    g_ctx.map_name = g_map.c_str();
    if (!g_core_load(&g_ctx)) {
        fpvlog::write("loader: the core failed to start; it stays loaded but idle");
        return;
    }
    {
        std::unique_lock<std::shared_mutex> lock(g_render_lock);
        g_core_present = reinterpret_cast<PresentFn>(get("p2fpv_core_present"));
        g_core_device_reset = reinterpret_cast<VoidFn>(get("p2fpv_core_device_reset"));
    }
    fpvlog::write("loader: core loaded (%s)", live.c_str());
}

// --- the renderer ---------------------------------------------------------------------

std::atomic<int> g_presents{0};

void on_present(void *device, void *) {
    if (g_presents.fetch_add(1) == 0) fpvlog::write("loader: first frame presented");
    std::shared_lock<std::shared_mutex> lock(g_render_lock);
    if (VoidFn release = g_release_pending) {
        release();
        g_release_pending = nullptr;  // only ever cleared here, under the shared lock: one presenting thread
    }
    if (g_core_present) g_core_present(static_cast<IDirect3DDevice9 *>(device));
}

void on_reset(void *, void *) {
    std::shared_lock<std::shared_mutex> lock(g_render_lock);
    if (g_core_device_reset) g_core_device_reset();
}

// --- the developer bridge ----------------------------------------------------------

void *g_engine_client = nullptr;  // VEngineClient015

void exec(const char *cmd) {
    if (!g_engine_client) return;
    guest::Scratch scratch;
    guest::vcall<void>(g_engine_client, 104, guest::str(cmd));  // ExecuteClientCmd
}

// fpv-data/cmd.txt: each line is a console command, run on the main thread;
// the file is deleted afterwards and the commands noted in the log.
void poll_commands() {
    std::string path = paths::data_dir() + "/cmd.txt";
    FILE *f = std::fopen(path.c_str(), "rb");
    if (!f) return;
    std::string text;
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) text.append(buf, n);
    std::fclose(f);
    platform::remove_file(path);
    size_t start = 0;
    while (start < text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(start, end - start);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (!line.empty()) {
            fpvlog::write("bridge: %s", line.c_str());
            if (line == "reload_core") load_core();
            else exec(line.c_str());
        }
        start = end + 1;
    }
}

// Each time the game polls its window events (main thread, between frames,
// also at the main menu), as the Windows build's timer: a command run inside
// the server's GameFrame (`map`, `load`) dropped the game to the menu.
void periodic(void *) {
    static uint32_t last_commands = 0, last_check = 0;
    uint32_t now = platform::ticks_ms();
    if (now - last_check > 500) {
        last_check = now;
        if (core_changed()) {
            fpvlog::write("loader: new core build, reloading");
            platform::sleep_ms(300);  // let the build finish writing it
            core_changed();
            load_core();
        }
    }
    if (now - last_commands > 200) {
        last_commands = now;
        poll_commands();
    }
}

void start_console_log() {
    // relative to the game folder (portal2/); the console mirrors to it
    exec("con_logfile addons/portal2-fpv/fpv-data/console.log");
}

void libfpv_log(void *, fpv_log_level level, const char *message) {
    static const char *names[] = {"debug", "info", "warn", "error"};
    fpvlog::write("libfpv %s: %s", names[level & 3], message);
}

// --- the plugin (IServerPluginCallbacks, version 3) ----------------------------------
// Methods are called as cdecl with `this` first (GCC, 32-bit Linux).

guest::addr arg(guest::addr args, int i) { return guest::r32(args + 4 * uint32_t(i)); }
const RtPluginHost *host() { return guest::host; }

void plugin_load(void *cpu, guest::addr args, void *) {
    fpvlog::write("loader: portal2-fpv %s %s (macOS, source-recomp), in %s", __DATE__, __TIME__, paths::mod_dir().c_str());
    g_ctx.size = sizeof g_ctx;
    g_ctx.engine_factory = arg(args, 1);
    g_ctx.server_factory = arg(args, 2);
    g_ctx.host = guest::host;
    g_ctx.libfpv_log = &libfpv_log;
    g_ctx.in_level = false;
    g_ctx.map_name = "";
    {
        guest::Scratch scratch;
        g_engine_client = guest::call<void *>(g_ctx.engine_factory, guest::str("VEngineClient015"),
                                              static_cast<int *>(nullptr));
    }
    if (!g_engine_client) fpvlog::write("loader: no VEngineClient015");
    host()->set_present_hook(g_plugin, &on_present, nullptr);
    host()->set_reset_hook(g_plugin, &on_reset, nullptr);
    start_console_log();
    core_changed();  // remember the timestamp
    load_core();
    host()->set_events_hook(g_plugin, &periodic, nullptr);
    host()->set_return(cpu, 1);
}

void plugin_unload(void *, guest::addr, void *) {
    host()->set_events_hook(g_plugin, nullptr, nullptr);
    unload_core();
    host()->set_present_hook(g_plugin, nullptr, nullptr);
    host()->set_reset_hook(g_plugin, nullptr, nullptr);
    fpvlog::write("loader: unloaded");
}

guest::addr g_description = 0;
void plugin_description(void *cpu, guest::addr, void *) { host()->set_return(cpu, g_description); }

void plugin_level_init(void *, guest::addr args, void *) {
    guest::addr map = arg(args, 1);
    g_map = map ? guest::str_at(map) : "";
    g_ctx.map_name = g_map.c_str();
    g_ctx.in_level = true;
    fpvlog::write("loader: level %s", g_map.c_str());
    if (g_core_level_init) g_core_level_init(g_map.c_str());
}

void plugin_server_activate(void *, guest::addr, void *) {
    if (g_core_server_activate) g_core_server_activate();
}

void plugin_game_frame(void *, guest::addr args, void *) {
    if (g_core_game_frame) g_core_game_frame((arg(args, 1) & 0xff) != 0);
}

void plugin_level_shutdown(void *, guest::addr, void *) {
    if (g_core_level_shutdown) g_core_level_shutdown();
    g_ctx.in_level = false;
    g_map.clear();
    g_ctx.map_name = g_map.c_str();
}

void plugin_nothing(void *cpu, guest::addr, void *) { host()->set_return(cpu, 0); }  // PLUGIN_CONTINUE

guest::addr g_object = 0;  // the plugin as the engine sees it

guest::addr make_object() {
    struct Slot {
        const char *name;
        RtPluginFn fn;
    };
    static const Slot slots[] = {
        {"Load", &plugin_load}, {"Unload", &plugin_unload}, {"Pause", &plugin_nothing}, {"UnPause", &plugin_nothing},
        {"GetPluginDescription", &plugin_description}, {"LevelInit", &plugin_level_init},
        {"ServerActivate", &plugin_server_activate}, {"GameFrame", &plugin_game_frame},
        {"LevelShutdown", &plugin_level_shutdown}, {"ClientActive", &plugin_nothing},
        {"ClientFullyConnect", &plugin_nothing}, {"ClientDisconnect", &plugin_nothing},
        {"ClientPutInServer", &plugin_nothing}, {"SetCommandClient", &plugin_nothing},
        {"ClientSettingsChanged", &plugin_nothing}, {"ClientConnect", &plugin_nothing},
        {"ClientCommand", &plugin_nothing}, {"NetworkIDValidated", &plugin_nothing},
        {"OnQueryCvarValueFinished", &plugin_nothing}, {"OnEdictAllocated", &plugin_nothing},
        {"OnEdictFreed", &plugin_nothing},
    };
    constexpr size_t n = sizeof slots / sizeof slots[0];
    guest::addr vtable = host()->alloc(4 * n), object = host()->alloc(16);
    if (!vtable || !object) return 0;
    for (size_t i = 0; i < n; ++i) {
        std::string name = std::string("p2fpv.Plugin.") + slots[i].name;
        guest::addr fn = host()->define(name.c_str(), slots[i].fn, nullptr);
        std::memcpy(guest::h(vtable + 4 * uint32_t(i)), &fn, 4);
    }
    std::memcpy(guest::h(object), &vtable, 4);
    const char desc[] = "portal2-fpv: an FPV drone (libfpv)";
    g_description = host()->alloc(sizeof desc);
    if (g_description) std::memcpy(guest::h(g_description), desc, sizeof desc);
    return object;
}

// CreateInterface(const char *name, int *returnCode)
void create_interface(void *cpu, guest::addr args, void *) {
    guest::addr name = arg(args, 0), rc = arg(args, 1);
    // the layout above is version 3 (the engine asks for the newest it knows first)
    bool ours = name && std::strcmp(guest::str_at(name), "ISERVERPLUGINCALLBACKS003") == 0;
    if (ours && !g_object) g_object = make_object();
    ours = ours && g_object;
    if (rc) {
        int32_t v = ours ? 0 : 1;
        std::memcpy(guest::h(rc), &v, 4);
    }
    host()->set_return(cpu, ours ? g_object : 0);
}

}  // namespace

extern "C" __attribute__((visibility("default"))) int rt_native_plugin_init(const RtPluginHost *h, RtPlugin *plugin) {
    if (!h || h->size < sizeof(RtPluginHost) || h->version < RT_PLUGIN_HOST_VERSION) return 1;
    guest::host = h;
    g_plugin = plugin;
    guest::addr fn = h->define("p2fpv.CreateInterface", &create_interface, nullptr);
    if (!fn) return 1;
    h->export_symbol(plugin, "CreateInterface", fn);
    return 0;
}
