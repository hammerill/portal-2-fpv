#include "platform.h"

#include <windows.h>

#include <cstring>

#include "guest.h"

namespace platform {

uint32_t ticks_ms() { return GetTickCount(); }

double seconds() {
    LARGE_INTEGER now, freq;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    return double(now.QuadPart) / double(freq.QuadPart);
}

void sleep_ms(unsigned ms) { Sleep(ms); }

void local_time(int &h, int &m, int &s, int &ms) {
    SYSTEMTIME t;
    GetLocalTime(&t);
    h = t.wHour, m = t.wMinute, s = t.wSecond, ms = t.wMilliseconds;
}

bool file_exists(const std::string &path) { return GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES; }
void make_dir(const std::string &path) { CreateDirectoryA(path.c_str(), nullptr); }
void remove_file(const std::string &path) { DeleteFileA(path.c_str()); }
void replace_file(const std::string &from, const std::string &to) {
    MoveFileExA(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING);
}

uint64_t file_stamp(const std::string &path) {
    WIN32_FILE_ATTRIBUTE_DATA data;
    if (!GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &data)) return 0;
    return (uint64_t(data.ftLastWriteTime.dwHighDateTime) << 32) | data.ftLastWriteTime.dwLowDateTime;
}

std::string module_dir(const void *address) {
    HMODULE self = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       static_cast<LPCSTR>(address), &self);
    char path[MAX_PATH];
    DWORD n = GetModuleFileNameA(self, path, MAX_PATH);
    std::string dir(path, n);
    return dir.substr(0, dir.find_last_of("\\/"));
}

}  // namespace platform

namespace input {
namespace {

// Set-1 scan codes of the letters and digits (their physical place).
const UINT kScan[] = {0x1E, 0x30, 0x2E, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17, 0x24, 0x25, 0x26, 0x32,
                      0x31, 0x18, 0x19, 0x10, 0x13, 0x1F, 0x14, 0x16, 0x2F, 0x11, 0x2D, 0x15, 0x2C,
                      0x02, 0x03};
const int kVk[] = {VK_RETURN, VK_LEFT, VK_RIGHT, VK_UP, VK_DOWN, VK_F7, VK_F8, VK_ESCAPE, VK_TAB, VK_BACK, VK_SPACE, VK_DELETE};

HWND game_window() {
    HWND w = GetForegroundWindow();
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    return pid == GetCurrentProcessId() ? w : nullptr;
}

int vk_of(Key k) {
    int i = int(k);
    if (i < int(Key::Enter)) {
        HKL layout = GetKeyboardLayout(GetWindowThreadProcessId(GetForegroundWindow(), nullptr));
        return int(MapVirtualKeyExW(kScan[i], MAPVK_VSC_TO_VK, layout));
    }
    return kVk[i - int(Key::Enter)];
}

// Source pins the OS cursor to the window's centre every frame (inputsystem.dll's
// SetCursorPosition, vgui2's too), even with cl_mouseenable 0, so the overlay's
// cursor could not move. While the overlay is open those modules' SetCursorPos
// imports point here and do nothing.
BOOL WINAPI no_set_cursor_pos(int, int) { return TRUE; }

void **find_import(HMODULE m, const char *dll, const char *fn) {
    auto base = reinterpret_cast<uint8_t *>(m);
    auto dos = reinterpret_cast<IMAGE_DOS_HEADER *>(base);
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS *>(base + dos->e_lfanew);
    auto &dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return nullptr;
    for (auto d = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR *>(base + dir.VirtualAddress); d->Name; ++d) {
        if (_stricmp(reinterpret_cast<char *>(base + d->Name), dll) != 0 || !d->OriginalFirstThunk) continue;
        auto names = reinterpret_cast<IMAGE_THUNK_DATA *>(base + d->OriginalFirstThunk);
        auto slots = reinterpret_cast<IMAGE_THUNK_DATA *>(base + d->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++slots) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            auto by_name = reinterpret_cast<IMAGE_IMPORT_BY_NAME *>(base + names->u1.AddressOfData);
            if (strcmp(reinterpret_cast<char *>(by_name->Name), fn) == 0) return reinterpret_cast<void **>(&slots->u1.Function);
        }
    }
    return nullptr;
}

struct CursorPin {
    void **slot = nullptr;
    void *original = nullptr;
};
CursorPin g_cursor_pins[2];

void write_slot(void **slot, void *value) { guest::patch(guest::g(slot), &value, sizeof value); }

}  // namespace

bool key_down(Key k) {
    int vk = vk_of(k);
    return vk && (GetAsyncKeyState(vk) & 0x8000) != 0;
}

bool mouse_down(int button) {
    const int vk[3] = {VK_LBUTTON, VK_RBUTTON, VK_MBUTTON};
    return button >= 0 && button < 3 && (GetAsyncKeyState(vk[button]) & 0x8000) != 0;
}

bool focused() { return game_window() != nullptr; }

bool cursor(float w, float h, float &x, float &y) {
    HWND wnd = game_window();
    POINT p;
    RECT rc;
    if (!wnd || !GetCursorPos(&p) || !ScreenToClient(wnd, &p) || !GetClientRect(wnd, &rc)) return false;
    float sx = rc.right > 0 ? w / float(rc.right) : 1.0f, sy = rc.bottom > 0 ? h / float(rc.bottom) : 1.0f;
    x = float(p.x) * sx, y = float(p.y) * sy;
    return true;
}

void free_cursor(bool free) {
    static const char *const modules[] = {"inputsystem.dll", "vgui2.dll"};
    for (int i = 0; i < 2; ++i) {
        CursorPin &p = g_cursor_pins[i];
        if (free && !p.slot) {
            HMODULE m = GetModuleHandleA(modules[i]);
            void **slot = m ? find_import(m, "user32.dll", "SetCursorPos") : nullptr;
            if (!slot) continue;
            p.slot = slot;
            p.original = *slot;
            write_slot(slot, reinterpret_cast<void *>(&no_set_cursor_pos));
        } else if (!free && p.slot) {
            write_slot(p.slot, p.original);
            p = {};
        }
    }
}

}  // namespace input
