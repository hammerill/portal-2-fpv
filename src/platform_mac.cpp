// macOS (source-recomp): the game's window and input are SDL2's, in the same
// process as the mod. The mod reads them from that SDL2 (the one the runtime
// loaded, found by name among the loaded images: libfpv embeds an SDL3 of its
// own, hidden, which never sees the game's window).
#include "platform.h"

#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <mach/mach_time.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <ctime>

#include "guest.h"
#include "log.h"

namespace platform {

uint32_t ticks_ms() { return uint32_t(seconds() * 1000.0); }

double seconds() {
    static mach_timebase_info_data_t base;
    if (!base.denom) mach_timebase_info(&base);
    return double(mach_absolute_time()) * base.numer / base.denom / 1e9;
}

void sleep_ms(unsigned ms) { usleep(ms * 1000); }

void local_time(int &h, int &m, int &s, int &ms) {
    timeval tv;
    gettimeofday(&tv, nullptr);
    tm t;
    localtime_r(&tv.tv_sec, &t);
    h = t.tm_hour, m = t.tm_min, s = t.tm_sec, ms = int(tv.tv_usec / 1000);
}

bool file_exists(const std::string &path) { return access(path.c_str(), F_OK) == 0; }
void make_dir(const std::string &path) { mkdir(path.c_str(), 0755); }
void remove_file(const std::string &path) { unlink(path.c_str()); }
void replace_file(const std::string &from, const std::string &to) { std::rename(from.c_str(), to.c_str()); }

uint64_t file_stamp(const std::string &path) {
    struct stat st;
    if (stat(path.c_str(), &st)) return 0;
    return uint64_t(st.st_mtimespec.tv_sec) * 1000000000ull + uint64_t(st.st_mtimespec.tv_nsec);
}

std::string module_dir(const void *address) {
    Dl_info info{};
    if (!dladdr(address, &info) || !info.dli_fname) return ".";
    std::string path = info.dli_fname;
    return path.substr(0, path.find_last_of('/'));
}

}  // namespace platform

namespace input {
namespace {

// SDL2's scancodes (USB HID usages): letters 4..29, 1 and 2 at 30 and 31.
const int kSdlScancode[] = {4,  5,  6,  7,  8,  9,  10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23,
                            24, 25, 26, 27, 28, 29, 30, 31,
                            40 /*Return*/, 80 /*Left*/, 79 /*Right*/, 82 /*Up*/, 81 /*Down*/, 64 /*F7*/, 65 /*F8*/,
                            41 /*Escape*/, 43 /*Tab*/, 42 /*Backspace*/, 44 /*Space*/, 76 /*Delete*/};
static_assert(sizeof kSdlScancode / sizeof kSdlScancode[0] == int(Key::Count), "a scancode per key");

struct Sdl {
    const uint8_t *(*GetKeyboardState)(int *) = nullptr;
    uint32_t (*GetMouseState)(int *, int *) = nullptr;
    void *(*GetKeyboardFocus)() = nullptr;
    void *(*GetMouseFocus)() = nullptr;
    void (*GetWindowSize)(void *, int *, int *) = nullptr;
    bool ok = false;
};

const Sdl &sdl() {
    static Sdl s;
    static bool tried = false;
    if (tried) return s;
    tried = true;
    void *lib = nullptr;
    for (uint32_t i = 0, n = _dyld_image_count(); i < n && !lib; ++i) {
        const char *name = _dyld_get_image_name(i);
        const char *base = name ? std::strrchr(name, '/') : nullptr;
        if (base && std::strncmp(base + 1, "libSDL2", 7) == 0) lib = dlopen(name, RTLD_NOW | RTLD_NOLOAD);
    }
    if (!lib) {
        fpvlog::write("input: the game's SDL2 is not loaded; no keyboard or mouse for the drone");
        return s;
    }
#define SDL_FN(name) s.name = reinterpret_cast<decltype(s.name)>(dlsym(lib, "SDL_" #name))
    SDL_FN(GetKeyboardState);
    SDL_FN(GetMouseState);
    SDL_FN(GetKeyboardFocus);
    SDL_FN(GetMouseFocus);
    SDL_FN(GetWindowSize);
#undef SDL_FN
    s.ok = s.GetKeyboardState && s.GetMouseState && s.GetKeyboardFocus && s.GetMouseFocus && s.GetWindowSize;
    if (!s.ok) fpvlog::write("input: SDL2 lacks a function the mod reads");
    return s;
}

}  // namespace

bool key_down(Key k) {
    const Sdl &s = sdl();
    if (!s.ok || k >= Key::Count) return false;
    const uint8_t *state = s.GetKeyboardState(nullptr);
    return state && state[kSdlScancode[int(k)]] != 0;
}

bool mouse_down(int button) {
    const Sdl &s = sdl();
    if (!s.ok || button < 0 || button > 2) return false;
    static const uint32_t mask[3] = {1u << 0, 1u << 2, 1u << 1};  // SDL_BUTTON(1), (3), (2)
    return (s.GetMouseState(nullptr, nullptr) & mask[button]) != 0;
}

bool focused() {
    const Sdl &s = sdl();
    return s.ok && s.GetKeyboardFocus() != nullptr;
}

bool cursor(float w, float h, float &x, float &y) {
    const Sdl &s = sdl();
    if (!s.ok) return false;
    void *window = s.GetMouseFocus();
    if (!window) return false;
    int px = 0, py = 0, ww = 0, wh = 0;
    s.GetMouseState(&px, &py);  // in the window's points
    s.GetWindowSize(window, &ww, &wh);
    if (ww <= 0 || wh <= 0) return false;
    x = float(px) * w / float(ww), y = float(py) * h / float(wh);
    return true;
}

void free_cursor(bool free) {
    if (guest::host && guest::host->release_mouse) guest::host->release_mouse(free ? 1 : 0);
}

}  // namespace input
