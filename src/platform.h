// What differs between Windows (the game's own 32-bit DLLs) and macOS (the
// recompiled Linux game, source-recomp): time, files, the keyboard and mouse.
#pragma once

#include <cstdint>
#include <string>

namespace platform {

uint32_t ticks_ms();  // monotonic milliseconds
double seconds();     // monotonic seconds
void sleep_ms(unsigned ms);
void local_time(int &h, int &m, int &s, int &ms);

bool file_exists(const std::string &path);
void make_dir(const std::string &path);
void remove_file(const std::string &path);
void replace_file(const std::string &from, const std::string &to);  // a rename over `to`
// Modification time (any unit, compared for equality), 0 if missing.
uint64_t file_stamp(const std::string &path);
// The folder holding the module that contains `address` (the loader, the core).
std::string module_dir(const void *address);

}  // namespace platform

namespace input {

// Keys the mod reads. Letters and digits by their physical place on a US
// keyboard (libfpv's sticks are W A S D wherever the letters are).
enum class Key : uint8_t {
    A, B, C, D, E, F, G, H, I, J, K, L, M, N, O, P, Q, R, S, T, U, V, W, X, Y, Z,
    D1, D2,
    Enter, Left, Right, Up, Down, F7, F8, Escape, Tab, Backspace, Space, Delete,
    Count
};
inline Key letter(char c) { return Key(int(Key::A) + (c - 'A')); }

bool key_down(Key k);
bool mouse_down(int button);  // 0 left, 1 right, 2 middle
bool focused();               // the game's window has the keyboard
// The cursor in the game's back buffer (w x h pixels); false if unknown.
bool cursor(float w, float h, float &x, float &y);
// Free the OS cursor from the game (the F8 overlay's mouse), or give it back.
void free_cursor(bool free);

}  // namespace input
