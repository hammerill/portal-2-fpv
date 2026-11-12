#include "guest.h"

#include <string>

#include "log.h"

#if P2FPV_RECOMP
#else
#include <windows.h>
#endif

namespace guest {

// --- scratch ---------------------------------------------------------------------------

namespace {
constexpr size_t kScratchSize = 256 * 1024;
uint8_t *g_scratch = nullptr;
size_t g_top = 0;

uint8_t *scratch_base() {
    if (g_scratch) return g_scratch;
#if P2FPV_RECOMP
    addr a = host->alloc(kScratchSize);
    if (!a) {
        fpvlog::write("guest: no scratch memory in the game");
        return nullptr;
    }
    g_scratch = static_cast<uint8_t *>(h(a));
#else
    alignas(16) static uint8_t buffer[kScratchSize];
    g_scratch = buffer;
#endif
    return g_scratch;
}
}  // namespace

void *scratch(size_t size, size_t align) {
    uint8_t *base = scratch_base();
    size_t at = (g_top + align - 1) & ~(align - 1);
    if (!base || at + size > kScratchSize) {
        // never expected (a few KB at a time): a leaked Scratch, or recursion
        fpvlog::write("guest: scratch exhausted (%zu + %zu)", at, size);
        static uint8_t *spare = nullptr;
        return spare;
    }
    g_top = at + size;
    std::memset(base + at, 0, size);
    return base + at;
}

Scratch::Scratch() : mark_(g_top) {}
Scratch::~Scratch() { g_top = mark_; }

const char *str(const char *s) {
    size_t n = std::strlen(s) + 1;
    char *p = static_cast<char *>(scratch(n, 1));
    if (p) std::memcpy(p, s, n);
    return p;
}

// --- memory and modules -------------------------------------------------------------------

#if P2FPV_RECOMP

const RtPluginHost *host = nullptr;

bool readable(addr a, size_t size) { return a && host->readable(a, uint32_t(size)); }

bool patch(addr a, const void *bytes, size_t size) { return host->patch(a, bytes, uint32_t(size)) != 0; }

const char *module_file(const char *module) {
    static std::string names[8];
    static int next = 0;
    std::string &s = names[next++ % 8];
    s = module;
    // the engine's helper libraries carry a lib prefix on Linux
    if (s == "vstdlib" || s == "tier0") s = "lib" + s;
    s += ".so";
    return s.c_str();
}

bool module_range(const char *module, addr &base, size_t &size) {
    uint32_t b = 0, n = 0;
    if (!host->module_range(module_file(module), &b, &n)) return false;
    base = b, size = n;
    return true;
}

addr module_symbol(const char *module, const char *name) { return host->module_symbol(module_file(module), name); }

void *cpu() { return host->current_cpu(); }

uint32_t raw_call(addr fn, const uint32_t *words, size_t count, bool returns_float, double *st0) {
    void *c = host->current_cpu();
    if (!c || !fn) {
        static int reported = 0;
        if (reported++ < 5) fpvlog::write("guest: call to %08x %s", fn, c ? "(null)" : "outside a game thread");
        return 0;
    }
    return host->call(c, fn, words, count, returns_float ? 1 : 0, nullptr, st0);
}

#else

bool readable(addr a, size_t size) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!a || !VirtualQuery(h(a), &mbi, sizeof mbi)) return false;
    if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return false;
    return a + size <= reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
}

bool patch(addr a, const void *bytes, size_t size) {
    DWORD old;
    if (!VirtualProtect(h(a), size, PAGE_EXECUTE_READWRITE, &old)) return false;
    std::memcpy(h(a), bytes, size);
    VirtualProtect(h(a), size, old, &old);
    return true;
}

const char *module_file(const char *module) {
    static std::string names[8];
    static int next = 0;
    std::string &s = names[next++ % 8];
    s = std::string(module) + ".dll";
    return s.c_str();
}

bool module_range(const char *module, addr &base, size_t &size) {
    HMODULE m = GetModuleHandleA(module_file(module));
    if (!m) return false;
    auto *dos = reinterpret_cast<IMAGE_DOS_HEADER *>(m);
    auto *nt = reinterpret_cast<IMAGE_NT_HEADERS *>(reinterpret_cast<uint8_t *>(m) + dos->e_lfanew);
    base = g(m);
    size = nt->OptionalHeader.SizeOfImage;
    return true;
}

addr module_symbol(const char *module, const char *name) {
    HMODULE m = GetModuleHandleA(module_file(module));
    return m ? g(reinterpret_cast<void *>(GetProcAddress(m, name))) : 0;
}

#endif

}  // namespace guest
