// Static thread-local storage for the hand-loaded program.
//
// Compiled code finds its TLS block through the thread's ThreadLocalStoragePointer
// array (TEB + 0x58). Windows always gives the main executable index 0 in that array,
// so compilers hard-code slot 0 for an .exe instead of reading _tls_index. claude.exe
// therefore needs its block in slot 0, where the app's own executable normally is.
//
// The array itself belongs to Windows, which tracks it and frees it when the thread
// ends, so only entry 0 is changed, and the original entry is always put back:
// - on threads that belong to the program, from when they start until they end;
// - on threads the program borrows (thread-pool callbacks, exception handlers that
//   run on whatever thread faulted), only while the program's code runs.
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
DWORD g_key = TLS_OUT_OF_INDEXES;
PIMAGE_TLS_CALLBACK* g_callbacks = nullptr;

struct ThreadTls {
    void* block = nullptr;     // the program's TLS block for this thread
    void* original = nullptr;  // what Windows had in slot 0
    void** own_vec = nullptr;  // only if the thread had no array and we made one
    bool owned = false;        // the thread belongs to the program
};

struct LdrEntry {
    LIST_ENTRY in_load_order;
    LIST_ENTRY in_memory_order;
    LIST_ENTRY in_init_order;
    void* dll_base;
    void* entry_point;
    ULONG size_of_image;
    UNICODE_STRING full_name;
    UNICODE_STRING base_name;
};

// Logs which loaded module Windows gave TLS slot 0, and the highest slot in use.
void describe_loaded_tls() {
    auto* peb = *reinterpret_cast<uint8_t**>(reinterpret_cast<uint8_t*>(NtCurrentTeb()) + kTebPeb);
    auto* ldr = *reinterpret_cast<uint8_t**>(peb + kPebLdr);
    auto* head = reinterpret_cast<LIST_ENTRY*>(ldr + kLdrInLoadOrder);
    long high = -1;
    std::string slot0 = "nothing";
    for (LIST_ENTRY* e = head->Flink; e != head; e = e->Flink) {
        auto* le = reinterpret_cast<LdrEntry*>(e);
        auto* b = static_cast<uint8_t*>(le->dll_base);
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
        if (idx == 0 && le->base_name.Buffer) {
            slot0 = narrow(std::wstring(le->base_name.Buffer, le->base_name.Length / sizeof(wchar_t)).c_str());
        }
    }
    log("TLS: Windows uses %ld slots; slot 0 normally belongs to %s", high + 1, slot0.c_str());
}

void*** teb_vector() {
    return reinterpret_cast<void***>(reinterpret_cast<uint8_t*>(NtCurrentTeb()) + kTebTlsPointer);
}

// Gets this thread's record, with a program TLS block ready. Null if memory ran out.
ThreadTls* thread_record() {
    auto* t = static_cast<ThreadTls*>(TlsGetValue(g_key));
    HANDLE heap = GetProcessHeap();
    if (!t) {
        t = static_cast<ThreadTls*>(HeapAlloc(heap, HEAP_ZERO_MEMORY, sizeof(ThreadTls)));
        if (!t) return nullptr;
        TlsSetValue(g_key, t);
    }
    if (!t->block) {
        auto* block = static_cast<uint8_t*>(HeapAlloc(heap, 0, g_template_size + g_zero_fill + 16));
        if (!block) return nullptr;
        memcpy(block, g_template, g_template_size);
        memset(block + g_template_size, 0, g_zero_fill);
        t->block = block;
    }
    void*** vec = teb_vector();
    if (!*vec) {
        // No module Windows loaded uses TLS, so there's no array yet: make a one-entry one.
        t->own_vec = static_cast<void**>(HeapAlloc(heap, HEAP_ZERO_MEMORY, sizeof(void*)));
        if (!t->own_vec) return nullptr;
        *vec = t->own_vec;
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
    *reinterpret_cast<ULONG*>(t->AddressOfIndex) = 0;
    g_has_tls = true;
    describe_loaded_tls();
    log("TLS: claude.exe's %zu byte block goes in slot 0", g_template_size);
    return true;
}

void tls_ensure_thread() {
    if (!g_has_tls) return;
    ThreadTls* t = thread_record();
    if (!t) {
        log("TLS: out of memory setting up thread %lu", GetCurrentThreadId());
        return;
    }
    if (t->owned) return;
    void** vec = *teb_vector();
    t->original = vec[0];
    vec[0] = t->block;
    t->owned = true;
}

void tls_thread_exit() {
    if (!g_has_tls) return;
    auto* t = static_cast<ThreadTls*>(TlsGetValue(g_key));
    if (!t) return;
    void*** vec = teb_vector();
    if (t->owned && *vec) (*vec)[0] = t->original;
    if (t->own_vec && *vec == t->own_vec) *vec = nullptr;
    HANDLE heap = GetProcessHeap();
    if (t->own_vec) HeapFree(heap, 0, t->own_vec);
    if (t->block) HeapFree(heap, 0, t->block);
    HeapFree(heap, 0, t);
    TlsSetValue(g_key, nullptr);
}

void* tls_enter() {
    if (!g_has_tls) return nullptr;
    ThreadTls* t = thread_record();
    if (!t || t->owned) return nullptr;
    void** vec = *teb_vector();
    void* previous = vec[0];
    vec[0] = t->block;
    return previous ? previous : reinterpret_cast<void*>(1);
}

void tls_leave(void* previous) {
    if (!previous) return;
    auto* t = static_cast<ThreadTls*>(TlsGetValue(g_key));
    if (!t || t->owned) return;
    void** vec = *teb_vector();
    if (vec) vec[0] = previous == reinterpret_cast<void*>(1) ? nullptr : previous;
}

void tls_run_callbacks(DWORD reason) {
    if (!g_callbacks) return;
    for (PIMAGE_TLS_CALLBACK* cb = g_callbacks; *cb; ++cb) {
        (*cb)(g_image.base, reason, nullptr);
    }
}

}  // namespace xb
