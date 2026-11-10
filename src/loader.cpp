// portal2-fpv.dll: the server plugin Portal 2 loads at startup
// (portal2/addons/portal2-fpv.vdf). A thin, stable shell around core.dll:
//
// - the log, and libfpv's log callback (libfpv keeps it until the game exits);
// - the IServerPluginCallbacks the engine calls, forwarded to the core;
// - hot reload: a new core.dll (its timestamp changes) is copied to
//   core.live.dll and swapped in at the next server frame, mid-level, so a
//   build can be tested without restarting the game;
// - the D3D9 Present / Reset hooks, which run on the render thread: they call
//   the core under a lock, so a reload never pulls the core out from under them;
// - the developer bridge: lines written to fpv-data/cmd.txt run as console
//   commands, and the console is logged to fpv-data/console.log.
#include <windows.h>
#include <d3d9.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>

#include "coreapi.h"
#include "log.h"
#include "paths.h"

using namespace coreapi;

namespace {

// --- the core ---------------------------------------------------------------------

HMODULE g_core = nullptr;
FILETIME g_core_time{};
LoaderContext g_ctx{};
std::string g_map;

LoadFn g_core_load = nullptr;
VoidFn g_core_unload = nullptr;
GameFrameFn g_core_game_frame = nullptr;
LevelInitFn g_core_level_init = nullptr;
VoidFn g_core_server_activate = nullptr;
VoidFn g_core_level_shutdown = nullptr;
PresentFn g_core_present = nullptr;  // guarded by g_render_lock
VoidFn g_core_device_reset = nullptr;
VoidFn g_core_render_release = nullptr;  // runs on the render thread before an unload
std::atomic<bool> g_release_pending{false};
SRWLOCK g_render_lock = SRWLOCK_INIT;
void **g_d3d_vtable_seen = nullptr;  // set once the D3D9 hooks are in

std::string core_path() { return paths::mod_dir() + "\\core.dll"; }
std::string live_path() { return paths::mod_dir() + "\\core.live.dll"; }

bool core_changed() {
    WIN32_FILE_ATTRIBUTE_DATA data;
    if (!GetFileAttributesExA(core_path().c_str(), GetFileExInfoStandard, &data)) return false;
    if (CompareFileTime(&data.ftLastWriteTime, &g_core_time) == 0) return false;
    g_core_time = data.ftLastWriteTime;
    return true;
}

void unload_core() {
    if (!g_core) return;
    // the core's D3D9 objects are released by the render thread, at its next Present
    if (g_core_render_release && g_d3d_vtable_seen) {
        g_release_pending = true;
        for (int i = 0; i < 50 && g_release_pending; i++) Sleep(10);
        if (g_release_pending) fpvlog::write("loader: no frame presented in 0.5 s; the core's D3D9 objects leak");
        g_release_pending = false;
    }
    AcquireSRWLockExclusive(&g_render_lock);  // waits for a Present in progress
    g_core_present = nullptr;
    g_core_device_reset = nullptr;
    g_core_render_release = nullptr;
    ReleaseSRWLockExclusive(&g_render_lock);
    if (g_core_unload) g_core_unload();
    FreeLibrary(g_core);
    g_core = nullptr;
    g_core_load = nullptr;
    g_core_unload = g_core_server_activate = g_core_level_shutdown = nullptr;
    g_core_game_frame = nullptr;
    g_core_level_init = nullptr;
    fpvlog::write("loader: core unloaded");
}

void load_core() {
    unload_core();
    if (!CopyFileA(core_path().c_str(), live_path().c_str(), FALSE)) {
        fpvlog::write("loader: cannot copy the core (error %lu)", GetLastError());
        return;
    }
    HMODULE m = LoadLibraryA(live_path().c_str());
    if (!m) {
        fpvlog::write("loader: cannot load %s (error %lu)", live_path().c_str(), GetLastError());
        return;
    }
    auto get = [m](const char *name) { return reinterpret_cast<void *>(GetProcAddress(m, name)); };
    auto load = reinterpret_cast<LoadFn>(get("p2fpv_core_load"));
    if (!load) {
        fpvlog::write("loader: the core exports no p2fpv_core_load");
        FreeLibrary(m);
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
    AcquireSRWLockExclusive(&g_render_lock);
    g_core_present = reinterpret_cast<PresentFn>(get("p2fpv_core_present"));
    g_core_device_reset = reinterpret_cast<VoidFn>(get("p2fpv_core_device_reset"));
    g_core_render_release = reinterpret_cast<VoidFn>(get("p2fpv_core_render_release"));
    ReleaseSRWLockExclusive(&g_render_lock);
    fpvlog::write("loader: core loaded");
}

// --- D3D9 ----------------------------------------------------------------------------
// The game's device is created by d3d9.dll, or by dxvk_d3d9.dll with -vulkan;
// Portal 2 makes it a D3D9Ex device. Current d3d9.dll gives every device its
// own copy of the vtable (on the heap), so a dummy device's vtable is no use:
// the dummies only tell which functions Present, PresentEx, Reset and ResetEx
// are. The game's device is the object, pointed to from shaderapidx9.dll's
// data, whose vtable has that Present; its own vtable's slots are swapped.

using PresentPtr = HRESULT(WINAPI *)(IDirect3DDevice9 *, const RECT *, const RECT *, HWND, const RGNDATA *);
using PresentExPtr = HRESULT(WINAPI *)(IDirect3DDevice9 *, const RECT *, const RECT *, HWND, const RGNDATA *, DWORD);
using ResetPtr = HRESULT(WINAPI *)(IDirect3DDevice9 *, D3DPRESENT_PARAMETERS *);
using ResetExPtr = HRESULT(WINAPI *)(IDirect3DDevice9 *, D3DPRESENT_PARAMETERS *, D3DDISPLAYMODEEX *);
constexpr int kResetSlot = 16, kPresentSlot = 17, kPresentExSlot = 121, kResetExSlot = 132;

// The module's own functions, from the dummy devices.
void *g_fn_present = nullptr, *g_fn_present_ex = nullptr, *g_fn_reset = nullptr, *g_fn_reset_ex = nullptr;
const char *g_d3d_name = "d3d9.dll";
// Where the game keeps its device pointer, and the device we hooked.
void **g_device_ref = nullptr;
void *g_device = nullptr;
void **g_vtable = nullptr;  // the hooked device's vtable
bool g_vtable_ex = false;

bool patch_slot(void **slot, void *fn) {
    DWORD old;
    if (!VirtualProtect(slot, sizeof(void *), PAGE_EXECUTE_READWRITE, &old)) return false;
    *slot = fn;
    VirtualProtect(slot, sizeof(void *), old, &old);
    return true;
}

std::atomic<int> g_presents{0};

void on_present(IDirect3DDevice9 *dev) {
    if (g_presents.fetch_add(1) == 0) fpvlog::write("loader: first frame presented");
    AcquireSRWLockShared(&g_render_lock);
    if (g_release_pending) {
        if (g_core_render_release) g_core_render_release();
        g_release_pending = false;
    } else if (g_core_present) {
        g_core_present(dev);
    }
    ReleaseSRWLockShared(&g_render_lock);
}

void on_reset() {
    AcquireSRWLockShared(&g_render_lock);
    if (g_core_device_reset) g_core_device_reset();
    ReleaseSRWLockShared(&g_render_lock);
}

// The originals are the module's functions themselves, the same for every device.
HRESULT WINAPI present_hook(IDirect3DDevice9 *dev, const RECT *src, const RECT *dst, HWND wnd, const RGNDATA *dirty) {
    on_present(dev);
    return reinterpret_cast<PresentPtr>(g_fn_present)(dev, src, dst, wnd, dirty);
}

HRESULT WINAPI present_ex_hook(IDirect3DDevice9 *dev, const RECT *src, const RECT *dst, HWND wnd, const RGNDATA *dirty,
                               DWORD flags) {
    on_present(dev);
    return reinterpret_cast<PresentExPtr>(g_fn_present_ex)(dev, src, dst, wnd, dirty, flags);
}

HRESULT WINAPI reset_hook(IDirect3DDevice9 *dev, D3DPRESENT_PARAMETERS *pp) {
    on_reset();
    return reinterpret_cast<ResetPtr>(g_fn_reset)(dev, pp);
}

HRESULT WINAPI reset_ex_hook(IDirect3DDevice9 *dev, D3DPRESENT_PARAMETERS *pp, D3DDISPLAYMODEEX *mode) {
    on_reset();
    return reinterpret_cast<ResetExPtr>(g_fn_reset_ex)(dev, pp, mode);
}

bool mem_readable(uintptr_t a, size_t n) {
    MEMORY_BASIC_INFORMATION m;
    if (!a || !VirtualQuery(reinterpret_cast<void *>(a), &m, sizeof m)) return false;
    if (m.State != MEM_COMMIT || (m.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return false;
    return a + n <= reinterpret_cast<uintptr_t>(m.BaseAddress) + m.RegionSize;
}

// Once, at load: the module's Present, PresentEx, Reset and ResetEx.
void learn_d3d9_functions() {
    g_d3d_name = GetModuleHandleA("dxvk_d3d9.dll") ? "dxvk_d3d9.dll" : "d3d9.dll";
    HMODULE mod = GetModuleHandleA(g_d3d_name);
    if (!mod) mod = LoadLibraryA(g_d3d_name);
    using CreateFn = IDirect3D9 *(WINAPI *)(UINT);
    using CreateExFn = HRESULT(WINAPI *)(UINT, IDirect3D9Ex **);
    auto create = mod ? reinterpret_cast<CreateFn>(reinterpret_cast<void *>(GetProcAddress(mod, "Direct3DCreate9"))) : nullptr;
    auto create_ex =
        mod ? reinterpret_cast<CreateExFn>(reinterpret_cast<void *>(GetProcAddress(mod, "Direct3DCreate9Ex"))) : nullptr;
    if (!create) {
        fpvlog::write("loader: no Direct3DCreate9 in %s", g_d3d_name);
        return;
    }
    WNDCLASSA wc{};
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "portal2-fpv-dummy";
    RegisterClassA(&wc);
    HWND wnd = CreateWindowA(wc.lpszClassName, "", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr, wc.hInstance, nullptr);
    D3DPRESENT_PARAMETERS pp{};
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.hDeviceWindow = wnd;
    pp.BackBufferFormat = D3DFMT_UNKNOWN;
    const DWORD flags = D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_DISABLE_DRIVER_MANAGEMENT;
    if (IDirect3D9 *d3d = create(D3D_SDK_VERSION)) {
        IDirect3DDevice9 *dev = nullptr;
        D3DPRESENT_PARAMETERS p = pp;
        if (SUCCEEDED(d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, wnd, flags, &p, &dev)) && dev) {
            void **vt = *reinterpret_cast<void ***>(dev);
            g_fn_present = vt[kPresentSlot];
            g_fn_reset = vt[kResetSlot];
            dev->Release();
        }
        d3d->Release();
    }
    IDirect3D9Ex *d3dex = nullptr;
    if (create_ex && SUCCEEDED(create_ex(D3D_SDK_VERSION, &d3dex)) && d3dex) {
        IDirect3DDevice9Ex *dev = nullptr;
        D3DPRESENT_PARAMETERS p = pp;
        if (SUCCEEDED(d3dex->CreateDeviceEx(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, wnd, flags, &p, nullptr, &dev)) && dev) {
            void **vt = *reinterpret_cast<void ***>(dev);
            if (!g_fn_present) g_fn_present = vt[kPresentSlot], g_fn_reset = vt[kResetSlot];
            g_fn_present_ex = vt[kPresentExSlot];
            g_fn_reset_ex = vt[kResetExSlot];
            dev->Release();
        }
        d3dex->Release();
    }
    DestroyWindow(wnd);
    fpvlog::write("loader: %s Present %p PresentEx %p", g_d3d_name, g_fn_present, g_fn_present_ex);
}

void unhook_device() {
    if (!g_vtable) return;
    // only while the game still holds that device: a freed one's vtable is gone
    if (g_device_ref && mem_readable(reinterpret_cast<uintptr_t>(g_device_ref), 4) && *g_device_ref == g_device) {
        patch_slot(&g_vtable[kPresentSlot], g_fn_present);
        patch_slot(&g_vtable[kResetSlot], g_fn_reset);
        if (g_vtable_ex) {
            patch_slot(&g_vtable[kPresentExSlot], g_fn_present_ex);
            patch_slot(&g_vtable[kResetExSlot], g_fn_reset_ex);
        }
    }
    g_vtable = nullptr;
    g_device = nullptr;
    g_d3d_vtable_seen = nullptr;
}

// Is the device at `ref` one of the module's (its Present, ours or the original)?
bool is_device(uintptr_t obj) {
    if (!mem_readable(obj, 4)) return false;
    uintptr_t vt = *reinterpret_cast<uintptr_t *>(obj);
    if (!mem_readable(vt, (kResetExSlot + 1) * sizeof(void *))) return false;
    void *p = reinterpret_cast<void **>(vt)[kPresentSlot];
    return p == g_fn_present || p == reinterpret_cast<void *>(&present_hook);
}

// Main thread, until found (and again if the game makes a new device): the
// game's device, from shaderapidx9.dll's data, hooked in its own vtable.
void hook_game_device() {
    if (!g_fn_present) return;
    static DWORD last_scan = 0;
    if (g_vtable && g_device_ref && *g_device_ref == g_device &&
        g_vtable[kPresentSlot] == reinterpret_cast<void *>(&present_hook))
        return;  // still ours
    if (g_vtable) {
        fpvlog::write("loader: the game's D3D9 device changed; hooking the new one");
        g_vtable = nullptr;  // the old device is gone: nothing to restore
        g_device = nullptr;
    }
    DWORD now = GetTickCount();
    if (last_scan && now - last_scan < 2000) return;
    last_scan = now;
    HMODULE api = GetModuleHandleA("shaderapidx9.dll");
    if (!api) return;
    auto *dos = reinterpret_cast<IMAGE_DOS_HEADER *>(api);
    auto *nt = reinterpret_cast<IMAGE_NT_HEADERS *>(reinterpret_cast<uint8_t *>(api) + dos->e_lfanew);
    auto *sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++, sec++) {
        if (!(sec->Characteristics & IMAGE_SCN_MEM_WRITE)) continue;  // .data / .bss
        uintptr_t a = reinterpret_cast<uintptr_t>(api) + sec->VirtualAddress;
        uintptr_t end = a + sec->Misc.VirtualSize;
        for (a = (a + 3) & ~uintptr_t(3); a + 4 <= end; a += 4) {
            uintptr_t obj = *reinterpret_cast<uintptr_t *>(a);
            if (obj < 0x10000 || !is_device(obj)) continue;
            void **vt = *reinterpret_cast<void ***>(obj);
            g_device_ref = reinterpret_cast<void **>(a);
            g_device = reinterpret_cast<void *>(obj);
            g_vtable = vt;
            g_vtable_ex = g_fn_present_ex && vt[kPresentExSlot] == g_fn_present_ex;
            bool ok = patch_slot(&vt[kResetSlot], reinterpret_cast<void *>(&reset_hook)) &&
                      patch_slot(&vt[kPresentSlot], reinterpret_cast<void *>(&present_hook));
            if (g_vtable_ex)
                ok = ok && patch_slot(&vt[kResetExSlot], reinterpret_cast<void *>(&reset_ex_hook)) &&
                     patch_slot(&vt[kPresentExSlot], reinterpret_cast<void *>(&present_ex_hook));
            g_d3d_vtable_seen = vt;
            fpvlog::write("loader: D3D9 hooks %s on the game's %sdevice %p (%s, shaderapidx9+%x)", ok ? "installed" : "FAILED",
                          g_vtable_ex ? "Ex " : "", g_device, g_d3d_name,
                          unsigned(a - reinterpret_cast<uintptr_t>(api)));
            return;
        }
    }
}

// --- the developer bridge ----------------------------------------------------------

void *g_engine_client = nullptr;  // VEngineClient015

void exec(const char *cmd) {
    if (!g_engine_client) return;
    using Fn = void(__thiscall *)(void *, const char *);
    (*reinterpret_cast<Fn **>(g_engine_client))[104](g_engine_client, cmd);  // ExecuteClientCmd
}

// fpv-data/cmd.txt: each line is a console command, run on the main thread;
// the file is deleted afterwards and the commands noted in the log.
void poll_commands() {
    static DWORD last = 0;
    DWORD now = GetTickCount();
    if (now - last < 200) return;
    last = now;
    std::string path = paths::data_dir() + "\\cmd.txt";
    FILE *f = std::fopen(path.c_str(), "rb");
    if (!f) return;
    std::string text;
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) text.append(buf, n);
    std::fclose(f);
    DeleteFileA(path.c_str());
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

// The bridge runs from a timer on the main thread, between frames: a command
// run inside the server's GameFrame (`map`, `load`) shut the server down from
// inside its own frame and dropped the game to the menu. The timer also runs at
// the main menu, where no GameFrame comes.
UINT_PTR g_timer = 0;
DWORD g_timer_tick = 0;  // GetTickCount() of the timer's last run

void periodic();

void CALLBACK bridge_timer(HWND, UINT, UINT_PTR, DWORD) {
    g_timer_tick = GetTickCount();
    periodic();
}

void start_console_log() {
    // relative to the game folder (portal2\); the console mirrors to it
    exec("con_logfile addons/portal2-fpv/fpv-data/console.log");
}

// --- the plugin ----------------------------------------------------------------------

class Plugin {
public:
    virtual bool Load(CreateInterfaceFn interfaceFactory, CreateInterfaceFn gameServerFactory) {
        fpvlog::write("loader: portal2-fpv %s %s, in %s", __DATE__, __TIME__, paths::mod_dir().c_str());
        g_ctx.size = sizeof g_ctx;
        g_ctx.engine_factory = interfaceFactory;
        g_ctx.server_factory = gameServerFactory;
        g_ctx.libfpv_log = &libfpv_log;
        g_ctx.in_level = false;
        g_ctx.map_name = "";
        g_engine_client = interfaceFactory ? interfaceFactory("VEngineClient015", nullptr) : nullptr;
        if (!g_engine_client) fpvlog::write("loader: no VEngineClient015");
        learn_d3d9_functions();
        hook_game_device();
        start_console_log();
        core_changed();  // remember the timestamp
        load_core();
        g_timer = SetTimer(nullptr, 0, 100, &bridge_timer);
        fpvlog::write("loader: bridge timer %s", g_timer ? "running" : "failed");
        return true;
    }
    virtual void Unload() {
        if (g_timer) KillTimer(nullptr, g_timer), g_timer = 0;
        unload_core();
        unhook_device();
        fpvlog::write("loader: unloaded");
    }
    virtual void Pause() {}
    virtual void UnPause() {}
    virtual const char *GetPluginDescription() { return "portal2-fpv: an FPV drone (libfpv)"; }
    virtual void LevelInit(const char *map) {
        g_map = map ? map : "";
        g_ctx.map_name = g_map.c_str();
        g_ctx.in_level = true;
        fpvlog::write("loader: level %s", g_map.c_str());
        if (g_core_level_init) g_core_level_init(g_map.c_str());
    }
    virtual void ServerActivate(void *, int, int) {
        if (g_core_server_activate) g_core_server_activate();
    }
    virtual void GameFrame(bool simulating) {
        if (GetTickCount() - g_timer_tick > 2000) periodic();  // the timer isn't running
        if (g_core_game_frame) g_core_game_frame(simulating);
    }
    virtual void LevelShutdown() {
        if (g_core_level_shutdown) g_core_level_shutdown();
        g_ctx.in_level = false;
        g_map.clear();
        g_ctx.map_name = g_map.c_str();
    }
    virtual void ClientFullyConnect(void *) {}
    virtual void ClientActive(void *) {}
    virtual void ClientDisconnect(void *) {}
    virtual void ClientPutInServer(void *, const char *) {}
    virtual void SetCommandClient(int) {}
    virtual void ClientSettingsChanged(void *) {}
    virtual int ClientConnect(bool *, void *, const char *, const char *, char *, int) { return 0; }
    virtual int ClientCommand(void *, const void *) { return 0; }
    virtual int NetworkIDValidated(const char *, const char *) { return 0; }
    virtual void OnQueryCvarValueFinished(int, void *, int, const char *, const char *) {}
    virtual void OnEdictAllocated(void *) {}
    virtual void OnEdictFreed(const void *) {}

    static void libfpv_log(void *, fpv_log_level level, const char *message) {
        static const char *names[] = {"debug", "info", "warn", "error"};
        fpvlog::write("libfpv %s: %s", names[level & 3], message);
    }
};

Plugin g_plugin;

void periodic() {
    static DWORD last_check = 0;
    DWORD now = GetTickCount();
    if (now - last_check > 500) {
        last_check = now;
        hook_game_device();
        if (core_changed()) {
            fpvlog::write("loader: new core build, reloading");
            Sleep(300);  // let the build finish writing it
            core_changed();
            load_core();
        }
    }
    poll_commands();
}

}  // namespace

extern "C" __declspec(dllexport) void *CreateInterface(const char *name, int *rc) {
    // the layout above is version 3 (the engine asks for the newest it knows first)
    if (name && std::strcmp(name, "ISERVERPLUGINCALLBACKS003") == 0) {
        if (rc) *rc = 0;
        return &g_plugin;
    }
    if (rc) *rc = 1;
    return nullptr;
}
