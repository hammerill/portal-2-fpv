#include "crash.h"

#include <windows.h>
#include <psapi.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>

#include "paths.h"

namespace crash {
namespace {

PVOID g_handler = nullptr;
std::atomic<const char *> g_note{"(none)"};
std::atomic<int> g_logged{0};

bool fatal(DWORD code) {
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION: case EXCEPTION_ILLEGAL_INSTRUCTION: case EXCEPTION_PRIV_INSTRUCTION:
    case EXCEPTION_INT_DIVIDE_BY_ZERO: case EXCEPTION_STACK_OVERFLOW: case 0xC0000409: case 0xC0000374:
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: case EXCEPTION_IN_PAGE_ERROR:
        return true;
    }
    return false;
}

// "module+offset" for an address inside a loaded module, or false.
bool where(DWORD addr, char *out, size_t n) {
    HMODULE m = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCSTR>(addr), &m) || !m)
        return false;
    char path[MAX_PATH] = "?";
    GetModuleFileNameA(m, path, MAX_PATH);
    const char *base = std::strrchr(path, '\\');
    std::snprintf(out, n, "%s+%lx", base ? base + 1 : path, addr - DWORD(m));
    return true;
}

bool executable(DWORD addr) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(reinterpret_cast<void *>(addr), &mbi, sizeof mbi)) return false;
    return mbi.State == MEM_COMMIT && (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE));
}

bool readable(const void *p, size_t n) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(p, &mbi, sizeof mbi)) return false;
    if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return false;
    return DWORD(p) + n <= DWORD(mbi.BaseAddress) + mbi.RegionSize;
}

LONG CALLBACK handler(EXCEPTION_POINTERS *ep) {
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (!fatal(code)) return EXCEPTION_CONTINUE_SEARCH;
    if (g_logged.fetch_add(1) >= 3) return EXCEPTION_CONTINUE_SEARCH;
    std::string path = paths::data_dir() + "\\crash.txt";
    FILE *f = std::fopen(path.c_str(), "a");
    if (!f) return EXCEPTION_CONTINUE_SEARCH;
    SYSTEMTIME t;
    GetLocalTime(&t);
    CONTEXT *c = ep->ContextRecord;
    char at[300] = "?";
    where(c->Eip, at, sizeof at);
    std::fprintf(f, "\n=== %04d-%02d-%02d %02d:%02d:%02d exception %08lx at %08lx %s (thread %lu)\n", t.wYear, t.wMonth,
                 t.wDay, t.wHour, t.wMinute, t.wSecond, code, c->Eip, at, GetCurrentThreadId());
    if (code == EXCEPTION_ACCESS_VIOLATION && ep->ExceptionRecord->NumberParameters >= 2)
        std::fprintf(f, "  %s %08lx\n", ep->ExceptionRecord->ExceptionInformation[0] == 0 ? "reading" :
                     ep->ExceptionRecord->ExceptionInformation[0] == 1 ? "writing" : "executing",
                     DWORD(ep->ExceptionRecord->ExceptionInformation[1]));
    std::fprintf(f, "  last step: %s\n", g_note.load());
    std::fprintf(f, "  eax %08lx ebx %08lx ecx %08lx edx %08lx esi %08lx edi %08lx ebp %08lx esp %08lx\n", c->Eax, c->Ebx,
                 c->Ecx, c->Edx, c->Esi, c->Edi, c->Ebp, c->Esp);
    std::fprintf(f, "  stack:\n");
    const DWORD *sp = reinterpret_cast<const DWORD *>(c->Esp);
    int found = 0;
    for (int i = 0; i < 4096 && found < 40; ++i) {
        if (!readable(sp + i, 4)) break;
        DWORD v = sp[i];
        char m[300];
        if (executable(v) && where(v, m, sizeof m)) {
            std::fprintf(f, "    [esp+%04x] %s\n", i * 4, m);
            ++found;
        }
    }
    std::fclose(f);
    return EXCEPTION_CONTINUE_SEARCH;
}

}  // namespace

void install() {
    if (!g_handler) g_handler = AddVectoredExceptionHandler(1, handler);
}

void uninstall() {
    if (g_handler) RemoveVectoredExceptionHandler(g_handler);
    g_handler = nullptr;
}

void note(const char *w) { g_note = w; }

}  // namespace crash
