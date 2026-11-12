// The game's memory and code, seen from the mod.
//
// Portal 2 is 32-bit on both platforms the mod runs on:
// - Windows: the mod is a 32-bit DLL in the game's process; a game pointer is
//   a plain pointer.
// - macOS (source-recomp): the game is the Linux build, recompiled for arm64,
//   with its own 4 GB address space; the mod is a native arm64 plugin
//   (src/rt_plugin.h). A game pointer is a 32-bit address in that space, and
//   memory + address is the same bytes for the mod.
//
// So engine structures keep their pointers as uint32_t (`addr`), turned into
// mod pointers with h() and back with g(); entities and interfaces are mod
// pointers to the game's bytes, which read and write as usual. Calls into the
// game go through vcall / call / thiscall: what the mod passes by pointer must
// live in the game's memory (scratch() and friends), which on Windows is
// any memory.
#pragma once

#include <cstdint>
#include <cstring>
#include <type_traits>

#if defined(__APPLE__)
#define P2FPV_RECOMP 1
#include "rt_plugin.h"
#else
#define P2FPV_RECOMP 0
#endif

namespace guest {

using addr = uint32_t;

#if P2FPV_RECOMP
extern const RtPluginHost *host;  // set by the loader (and handed to the core)
inline void *h(addr a) { return a ? host->memory + a : nullptr; }
inline addr g(const void *p) { return p ? addr(static_cast<const uint8_t *>(p) - host->memory) : 0; }
#else
inline void *h(addr a) { return reinterpret_cast<void *>(uintptr_t(a)); }
inline addr g(const void *p) { return addr(reinterpret_cast<uintptr_t>(p)); }
#endif

template <typename T>
T *at(addr a) {
    return static_cast<T *>(h(a));
}
inline addr r32(addr a) {
    addr v;
    std::memcpy(&v, h(a), 4);
    return v;
}
inline const char *str_at(addr a) { return static_cast<const char *>(h(a)); }

// The bytes are mapped (a pointer read from the game may be stale).
bool readable(addr a, size_t size);
// Writes over read-only game memory (vtables, constants).
bool patch(addr a, const void *bytes, size_t size);
// A game module's image ("server" names server.dll / server.so).
bool module_range(const char *module, addr &base, size_t &size);
// A module's exported function (CreateInterface), 0 if missing.
addr module_symbol(const char *module, const char *name);
// The module's file name on this platform: "server" -> server.dll / server.so.
const char *module_file(const char *module);

// --- scratch: the game's memory for what calls take by pointer -----------------------
// A stack: everything taken after a Scratch is given back when it ends.
void *scratch(size_t size, size_t align = 16);
class Scratch {
public:
    Scratch();
    ~Scratch();
    Scratch(const Scratch &) = delete;
    Scratch &operator=(const Scratch &) = delete;

private:
    size_t mark_;
};
template <typename T>
T *temp(const T &value = T{}) {
    void *p = scratch(sizeof(T), alignof(T) > 16 ? alignof(T) : 16);
    std::memcpy(p, static_cast<const void *>(&value), sizeof(T));
    return static_cast<T *>(p);
}
const char *str(const char *s);  // a copy in the game's memory

// --- calls ------------------------------------------------------------------------------
#if P2FPV_RECOMP
// The guest thread running the mod now (nullptr outside a game callback).
void *cpu();
template <typename T>
uint32_t word(T v) {
    if constexpr (std::is_same_v<T, std::nullptr_t>) return 0;
    else if constexpr (std::is_pointer_v<T>) return g(v);
    else if constexpr (std::is_same_v<T, float>) {
        uint32_t u;
        std::memcpy(&u, &v, 4);
        return u;
    } else {
        static_assert(std::is_integral_v<T> || std::is_enum_v<T>, "32-bit argument");
        return uint32_t(v);
    }
}
uint32_t raw_call(addr fn, const uint32_t *words, size_t count, bool returns_float, double *st0);
// cdecl, as GCC calls everything on 32-bit Linux (methods take `this` first).
template <typename R, typename... A>
R call(addr fn, A... args) {
    const uint32_t words[sizeof...(A) + 1] = {word(args)...};
    double st0 = 0;
    uint32_t eax = raw_call(fn, words, sizeof...(A), std::is_floating_point_v<R>, &st0);
    if constexpr (std::is_void_v<R>) return;
    else if constexpr (std::is_pointer_v<R>) return static_cast<R>(h(eax));
    else if constexpr (std::is_same_v<R, bool>) return (eax & 0xff) != 0;
    else if constexpr (std::is_floating_point_v<R>) return R(st0);
    else return R(eax);
}
template <typename R, typename... A>
R thiscall(addr fn, const void *self, A... args) {
    return call<R>(fn, self, args...);
}
#else
template <typename R, typename... A>
R call(addr fn, A... args) {
    return reinterpret_cast<R(__cdecl *)(A...)>(h(fn))(args...);
}
template <typename R, typename... A>
R thiscall(addr fn, const void *self, A... args) {
    return reinterpret_cast<R(__thiscall *)(const void *, A...)>(h(fn))(self, args...);
}
#endif

// Slot `index` of an object's vtable.
inline addr vfunc(const void *self, int index) { return r32(r32(g(self)) + 4 * uint32_t(index)); }

template <typename R, typename... A>
R vcall(const void *self, int index, A... args) {
    return thiscall<R>(vfunc(self, index), self, args...);
}

}  // namespace guest
