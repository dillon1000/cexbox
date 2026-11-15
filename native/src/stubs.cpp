// Generated stand-ins for imports the Xbox doesn't provide.
//
// Each stub is a small piece of machine code that loads a pointer to its description
// into rcx and jumps to stub_handler, which logs the call and returns a "not supported"
// result suited to the DLL it came from.
#include "xb.h"

#include <stdio.h>
#include <string.h>
#include <vector>

namespace xb {

namespace {

struct StubInfo {
    char dll[48];
    char name[80];
    uint64_t result;
    DWORD error;
    volatile LONG calls;
};

constexpr size_t kPoolSize = 64 * 1024;
constexpr size_t kThunkSize = 32;

uint8_t* g_pool = nullptr;
size_t g_pool_used = 0;
std::vector<uint8_t*> g_pending;

uint64_t stub_handler(StubInfo* s) {
    LONG n = InterlockedIncrement(&s->calls);
    if (n <= 3) log("called stub %s!%s (call %ld)", s->dll, s->name, n);
    SetLastError(s->error);
    return s->result;
}

uint64_t generic_stub() {
    SetLastError(ERROR_NOT_SUPPORTED);
    return 0;
}

bool same(const char* a, const char* b) { return _stricmp(a, b) == 0; }
bool starts(const char* s, const char* prefix) { return s && strncmp(s, prefix, strlen(prefix)) == 0; }

void pick_result(const char* dll, const char* name, StubInfo& s) {
    constexpr uint64_t kStatusNotImplemented = 0xC0000002;
    constexpr uint64_t kENotImpl = 0x80004001;
    constexpr uint64_t kMmNoDriver = 6;

    s.error = ERROR_NOT_SUPPORTED;
    s.result = 0;
    if (same(dll, "ntdll.dll") && (starts(name, "Nt") || starts(name, "Zw"))) {
        s.result = kStatusNotImplemented;
    } else if (starts(name, "Lsa") && !same(name, "LsaNtStatusToWinError")) {
        s.result = kStatusNotImplemented;
    } else if (same(dll, "fwpuclnt.dll") || same(dll, "netapi32.dll") || same(name ? name : "", "LsaNtStatusToWinError")) {
        s.result = ERROR_NOT_SUPPORTED;
    } else if (same(dll, "winmm.dll")) {
        s.result = kMmNoDriver;
    } else if (same(dll, "shell32.dll") || same(dll, "ole32.dll")) {
        s.result = kENotImpl;
    }
}

}  // namespace

void* make_stub(const char* dll, const char* name, uint16_t ordinal) {
    auto* s = static_cast<StubInfo*>(HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(StubInfo)));
    if (!s) return reinterpret_cast<void*>(&generic_stub);
    strncpy_s(s->dll, dll, _TRUNCATE);
    if (name) strncpy_s(s->name, name, _TRUNCATE);
    else snprintf(s->name, sizeof s->name, "#%u", ordinal);
    pick_result(dll, name, *s);

    if (!g_pool || g_pool_used + kThunkSize > kPoolSize) {
        g_pool = static_cast<uint8_t*>(VirtualAllocFromApp(nullptr, kPoolSize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        g_pool_used = 0;
        if (!g_pool) return reinterpret_cast<void*>(&generic_stub);
        g_pending.push_back(g_pool);
    }
    uint8_t* t = g_pool + g_pool_used;
    g_pool_used += kThunkSize;

    auto info = reinterpret_cast<uint64_t>(s);
    auto handler = reinterpret_cast<uint64_t>(&stub_handler);
    uint8_t code[kThunkSize] = {
        0x48, 0xB9, 0, 0, 0, 0, 0, 0, 0, 0,  // mov rcx, info
        0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0,  // mov rax, handler
        0xFF, 0xE0,                          // jmp rax
        0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC,
    };
    memcpy(code + 2, &info, 8);
    memcpy(code + 12, &handler, 8);
    memcpy(t, code, kThunkSize);
    return t;
}

bool stubs_finalize() {
    bool ok = true;
    for (uint8_t* pool : g_pending) {
        ULONG old = 0;
        if (!VirtualProtectFromApp(pool, kPoolSize, PAGE_EXECUTE_READ, &old)) {
            log("could not make stub pool executable: error %lu", GetLastError());
            ok = false;
        }
        FlushInstructionCache(GetCurrentProcess(), pool, kPoolSize);
    }
    g_pending.clear();
    g_pool = nullptr;  // later stubs start a new pool
    return ok;
}

}  // namespace xb
