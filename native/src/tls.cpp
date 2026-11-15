// Static thread-local storage for the hand-loaded program.
//
// Compiled code finds its TLS block through the thread's ThreadLocalStoragePointer
// array (TEB + 0x58), at the index stored in the image's _tls_index. Windows only
// fills that array for modules it loaded itself, so for claude.exe we pick an unused
// index, give each thread a larger copy of the array, and put claude.exe's block in it.
//
// Known limit: if Windows later loads another DLL with static TLS, it rebuilds the
// array at its own size and drops our entry for threads that already exist.
#include "xb.h"

#include <string.h>

namespace xb {

namespace {

bool g_has_tls = false;
const uint8_t* g_template = nullptr;
size_t g_template_size = 0;
size_t g_zero_fill = 0;
ULONG g_slot = 0;
size_t g_copy_count = 0;
DWORD g_mark = TLS_OUT_OF_INDEXES;
PIMAGE_TLS_CALLBACK* g_callbacks = nullptr;

constexpr size_t kTebTlsPointer = 0x58;
constexpr size_t kTebPeb = 0x60;
constexpr size_t kPebLdr = 0x18;
constexpr size_t kLdrInLoadOrder = 0x10;
constexpr ULONG kSpareSlots = 32;

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

}  // namespace

bool tls_init(Image& img, std::string& err) {
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

    long high = highest_loaded_tls_index();
    g_copy_count = static_cast<size_t>(high + 1);
    g_slot = static_cast<ULONG>(g_copy_count) + kSpareSlots;
    *reinterpret_cast<ULONG*>(t->AddressOfIndex) = g_slot;

    g_mark = TlsAlloc();
    if (g_mark == TLS_OUT_OF_INDEXES) {
        err = "Out of TLS slots";
        return false;
    }
    g_has_tls = true;
    log("TLS: %zu byte template, %zu zero fill, slot %lu (Windows uses %zu)", g_template_size, g_zero_fill,
        g_slot, g_copy_count);
    return true;
}

void tls_ensure_thread() {
    if (!g_has_tls || TlsGetValue(g_mark)) return;

    HANDLE heap = GetProcessHeap();
    auto* teb = reinterpret_cast<uint8_t*>(NtCurrentTeb());
    auto** old_vec = *reinterpret_cast<void***>(teb + kTebTlsPointer);
    size_t len = static_cast<size_t>(g_slot) + 1;

    // Windows frees this array with the process heap when the thread ends, so it must come from there.
    auto** vec = static_cast<void**>(HeapAlloc(heap, HEAP_ZERO_MEMORY, len * sizeof(void*)));
    auto* block = static_cast<uint8_t*>(HeapAlloc(heap, 0, g_template_size + g_zero_fill + 16));
    if (!vec || !block) {
        log("TLS: out of memory setting up thread %lu", GetCurrentThreadId());
        return;
    }
    if (old_vec && g_copy_count) memcpy(vec, old_vec, g_copy_count * sizeof(void*));
    memcpy(block, g_template, g_template_size);
    memset(block + g_template_size, 0, g_zero_fill);
    vec[g_slot] = block;
    *reinterpret_cast<void***>(teb + kTebTlsPointer) = vec;
    TlsSetValue(g_mark, reinterpret_cast<void*>(1));
}

void tls_run_callbacks(DWORD reason) {
    if (!g_callbacks) return;
    for (PIMAGE_TLS_CALLBACK* cb = g_callbacks; *cb; ++cb) {
        (*cb)(g_image.base, reason, nullptr);
    }
}

}  // namespace xb
