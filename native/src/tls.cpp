// Static thread-local storage for the hand-loaded program.
//
// Compiled code finds its TLS block through the thread's ThreadLocalStoragePointer
// array (TEB + 0x58). Windows always gives the main executable index 0 in that array,
// so compilers hard-code slot 0 for an .exe instead of reading _tls_index. claude.exe
// therefore needs its block in slot 0, where the app's own executable normally is.
//
// - Threads that belong to the program get their own copy of the array, with the
//   program's block in slot 0 for good.
// - Threads the program borrows (thread-pool callbacks, exception handlers that run
//   on whatever thread faulted) get the program's block in slot 0 only while its
//   code runs, and the app's own entry back afterwards.
#include "xb.h"

#include <string.h>

namespace xb {

namespace {

constexpr size_t kTebTlsPointer = 0x58;
constexpr size_t kTebPeb = 0x60;
constexpr size_t kPebLdr = 0x18;
constexpr size_t kLdrInLoadOrder = 0x10;

bool g_has_tls = false;
const uint8_t* g_template = nullptr;
size_t g_template_size = 0;
size_t g_zero_fill = 0;
size_t g_copy_count = 0;  // entries Windows uses in each thread's array
DWORD g_key = TLS_OUT_OF_INDEXES;
PIMAGE_TLS_CALLBACK* g_callbacks = nullptr;

struct ThreadTls {
    void** vec = nullptr;    // the array we installed for this thread
    size_t len = 0;
    void* block = nullptr;   // the program's TLS block for this thread
    bool owned = false;      // the thread belongs to the program
};

struct LdrEntry {
    LIST_ENTRY in_load_order;
    LIST_ENTRY in_memory_order;
    LIST_ENTRY in_init_order;
    void* dll_base;
};

// Highest TLS index Windows has handed out to modules loaded so far.
long highest_loaded_tls_index() {
    auto* peb = *reinterpret_cast<uint8_t**>(reinterpret_cast<uint8_t*>(NtCurrentTeb()) + kTebPeb);
    auto* ldr = *reinterpret_cast<uint8_t**>(peb + kPebLdr);
    auto* head = reinterpret_cast<LIST_ENTRY*>(ldr + kLdrInLoadOrder);
    long high = -1;
    for (LIST_ENTRY* e = head->Flink; e != head; e = e->Flink) {
        auto* b = static_cast<uint8_t*>(reinterpret_cast<LdrEntry*>(e)->dll_base);
        if (!b) continue;
        auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(b);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) continue;
        auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(b + dos->e_lfanew);
        auto& dd = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS];
        if (!dd.VirtualAddress || !dd.Size) continue;
        auto* t = reinterpret_cast<IMAGE_TLS_DIRECTORY64*>(b + dd.VirtualAddress);
        if (!t->AddressOfIndex) continue;
        long idx = *reinterpret_cast<long*>(t->AddressOfIndex);
        if (idx > high) high = idx;
    }
    return high;
}

void*** teb_vector() {
    return reinterpret_cast<void***>(reinterpret_cast<uint8_t*>(NtCurrentTeb()) + kTebTlsPointer);
}

// Makes sure this thread has an array we control, at least one entry long, and a
// program TLS block. Returns null if memory ran out.
ThreadTls* prepare_thread() {
    auto* t = static_cast<ThreadTls*>(TlsGetValue(g_key));
    HANDLE heap = GetProcessHeap();
    if (!t) {
        t = static_cast<ThreadTls*>(HeapAlloc(heap, HEAP_ZERO_MEMORY, sizeof(ThreadTls)));
        if (!t) return nullptr;
        TlsSetValue(g_key, t);
    }
    void*** slot = teb_vector();
    if (!t->vec || *slot != t->vec) {
        // Windows frees this array with the process heap when the thread ends, so it must come from there.
        size_t len = g_copy_count > 0 ? g_copy_count : 1;
        auto** vec = static_cast<void**>(HeapAlloc(heap, HEAP_ZERO_MEMORY, len * sizeof(void*)));
        if (!vec) return nullptr;
        if (*slot) memcpy(vec, *slot, g_copy_count * sizeof(void*));
        *slot = vec;
        t->vec = vec;
        t->len = len;
    }
    if (!t->block) {
        auto* block = static_cast<uint8_t*>(HeapAlloc(heap, 0, g_template_size + g_zero_fill + 16));
        if (!block) return nullptr;
        memcpy(block, g_template, g_template_size);
        memset(block + g_template_size, 0, g_zero_fill);
        t->block = block;
    }
    return t;
}

}  // namespace

bool tls_init(Image& img, std::string& err) {
    g_key = TlsAlloc();
    if (g_key == TLS_OUT_OF_INDEXES) {
        err = "Out of TLS slots";
        return false;
    }
    auto& dd = img.nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS];
    if (!dd.VirtualAddress || !dd.Size) {
        log("claude.exe has no static TLS");
        return true;
    }
    // The directory holds absolute addresses, already fixed up by relocation.
    auto* t = reinterpret_cast<IMAGE_TLS_DIRECTORY64*>(img.base + dd.VirtualAddress);
    g_template = reinterpret_cast<const uint8_t*>(t->StartAddressOfRawData);
    g_template_size = static_cast<size_t>(t->EndAddressOfRawData - t->StartAddressOfRawData);
    g_zero_fill = t->SizeOfZeroFill;
    g_callbacks = reinterpret_cast<PIMAGE_TLS_CALLBACK*>(t->AddressOfCallBacks);
    g_copy_count = static_cast<size_t>(highest_loaded_tls_index() + 1);
    *reinterpret_cast<ULONG*>(t->AddressOfIndex) = 0;
    g_has_tls = true;
    log("TLS: %zu byte template in slot 0 (Windows uses %zu slots)", g_template_size, g_copy_count);
    return true;
}

void tls_ensure_thread() {
    if (!g_has_tls) return;
    ThreadTls* t = prepare_thread();
    if (!t) {
        log("TLS: out of memory setting up thread %lu", GetCurrentThreadId());
        return;
    }
    t->vec[0] = t->block;
    t->owned = true;
}

void* tls_enter() {
    if (!g_has_tls) return nullptr;
    ThreadTls* t = prepare_thread();
    if (!t) return nullptr;
    if (t->owned) return nullptr;
    void* previous = t->vec[0];
    t->vec[0] = t->block;
    return previous ? previous : reinterpret_cast<void*>(1);
}

void tls_leave(void* previous) {
    if (!previous) return;
    auto* t = static_cast<ThreadTls*>(TlsGetValue(g_key));
    if (!t || t->owned || !t->vec) return;
    t->vec[0] = previous == reinterpret_cast<void*>(1) ? nullptr : previous;
}

void tls_run_callbacks(DWORD reason) {
    if (!g_callbacks) return;
    for (PIMAGE_TLS_CALLBACK* cb = g_callbacks; *cb; ++cb) {
        (*cb)(g_image.base, reason, nullptr);
    }
}

}  // namespace xb
