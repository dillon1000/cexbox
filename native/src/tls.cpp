// Static thread-local storage for the hand-loaded program.
//
// Compiled code finds its TLS block by reading the thread's TLS array pointer
// (mov reg, gs:[0x58]) and, for an .exe, taking entry 0, because Windows always
// gives the main executable index 0. In this app that slot belongs to a system DLL
// (KERNELBASE.dll), and the array itself is managed by Windows.
//
// So the loader leaves Windows' array alone and gives the program an array of its
// own: at load time, every "mov reg, gs:[0x58]" in the program's code is rewritten
// to read one of the thread's TLS slots (TEB TlsSlots[n], at gs:[0x1480 + 8n])
// instead, and that slot points at a one-entry array holding the program's block.
//
// - Threads that belong to the program get the slot set when they start.
// - Threads the program borrows (thread-pool callbacks, exception handlers that
//   run on whatever thread faulted) get it only while the program's code runs.
#include "xb.h"

#include <string.h>

namespace xb {

namespace {

constexpr DWORD kTebTlsSlots = 0x1480;  // TEB::TlsSlots[64]
constexpr DWORD kTebTlsPointer = 0x58;  // TEB::ThreadLocalStoragePointer

bool g_has_tls = false;
const uint8_t* g_template = nullptr;
size_t g_template_size = 0;
size_t g_zero_fill = 0;
DWORD g_slot = TLS_OUT_OF_INDEXES;  // the TEB slot patched code reads
DWORD g_key = TLS_OUT_OF_INDEXES;   // our per-thread record
PIMAGE_TLS_CALLBACK* g_callbacks = nullptr;

struct ThreadTls {
    void* vec[1] = {nullptr};  // what the program sees as its TLS array
    bool owned = false;        // the thread belongs to the program
};

// Rewrites "mov r64, gs:[0x58]" (65 REX.W 8B /r with an absolute disp32) in the
// image's code to read gs:[new_offset]. Returns how many were changed.
size_t patch_tls_reads(Image& img, DWORD new_offset) {
    size_t count = 0;
    auto* sec = IMAGE_FIRST_SECTION(img.nt);
    for (WORD i = 0; i < img.nt->FileHeader.NumberOfSections; ++i) {
        if (!(sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        uint8_t* p = img.base + sec[i].VirtualAddress;
        size_t n = sec[i].Misc.VirtualSize ? sec[i].Misc.VirtualSize : sec[i].SizeOfRawData;
        for (size_t j = 0; j + 9 <= n; ++j) {
            if (p[j] != 0x65) continue;
            uint8_t rex = p[j + 1], op = p[j + 2], modrm = p[j + 3];
            if ((rex != 0x48 && rex != 0x4C) || op != 0x8B || (modrm & 0xC7) != 0x04 || p[j + 4] != 0x25) continue;
            uint32_t disp;
            memcpy(&disp, p + j + 5, 4);
            if (disp != kTebTlsPointer) continue;
            memcpy(p + j + 5, &new_offset, 4);
            ++count;
            j += 8;
        }
    }
    return count;
}

ThreadTls* thread_record() {
    auto* t = static_cast<ThreadTls*>(TlsGetValue(g_key));
    if (t) return t;
    HANDLE heap = GetProcessHeap();
    t = static_cast<ThreadTls*>(HeapAlloc(heap, HEAP_ZERO_MEMORY, sizeof(ThreadTls)));
    auto* block = static_cast<uint8_t*>(HeapAlloc(heap, 0, g_template_size + g_zero_fill + 16));
    if (!t || !block) return nullptr;
    memcpy(block, g_template, g_template_size);
    memset(block + g_template_size, 0, g_zero_fill);
    t->vec[0] = block;
    TlsSetValue(g_key, t);
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
    // Only the first 64 slots live in the TEB itself, where patched code can reach them.
    g_slot = TlsAlloc();
    if (g_slot == TLS_OUT_OF_INDEXES || g_slot >= 64) {
        err = "No TEB TLS slot free for claude.exe";
        return false;
    }
    // The directory holds absolute addresses, already fixed up by relocation.
    auto* t = reinterpret_cast<IMAGE_TLS_DIRECTORY64*>(img.base + dd.VirtualAddress);
    g_template = reinterpret_cast<const uint8_t*>(t->StartAddressOfRawData);
    g_template_size = static_cast<size_t>(t->EndAddressOfRawData - t->StartAddressOfRawData);
    g_zero_fill = t->SizeOfZeroFill;
    g_callbacks = reinterpret_cast<PIMAGE_TLS_CALLBACK*>(t->AddressOfCallBacks);
    *reinterpret_cast<ULONG*>(t->AddressOfIndex) = 0;
    size_t patched = patch_tls_reads(img, kTebTlsSlots + 8 * g_slot);
    g_has_tls = true;
    log("TLS: %zu byte block; %zu TLS reads redirected to TEB slot %lu", g_template_size, patched, g_slot);
    return true;
}

void tls_ensure_thread() {
    if (!g_has_tls) return;
    ThreadTls* t = thread_record();
    if (!t) {
        log("TLS: out of memory setting up thread %lu", GetCurrentThreadId());
        return;
    }
    t->owned = true;
    TlsSetValue(g_slot, t->vec);
}

void tls_thread_exit() {
    if (!g_has_tls) return;
    auto* t = static_cast<ThreadTls*>(TlsGetValue(g_key));
    if (!t) return;
    TlsSetValue(g_slot, nullptr);
    TlsSetValue(g_key, nullptr);
    HANDLE heap = GetProcessHeap();
    if (t->vec[0]) HeapFree(heap, 0, t->vec[0]);
    HeapFree(heap, 0, t);
}

void* tls_enter() {
    if (!g_has_tls) return nullptr;
    ThreadTls* t = thread_record();
    if (!t || t->owned) return nullptr;
    void* previous = TlsGetValue(g_slot);
    TlsSetValue(g_slot, t->vec);
    return previous ? previous : reinterpret_cast<void*>(1);
}

void tls_leave(void* previous) {
    if (!previous) return;
    auto* t = static_cast<ThreadTls*>(TlsGetValue(g_key));
    if (!t || t->owned) return;
    TlsSetValue(g_slot, previous == reinterpret_cast<void*>(1) ? nullptr : previous);
}

void tls_run_callbacks(DWORD reason) {
    if (!g_callbacks) return;
    for (PIMAGE_TLS_CALLBACK* cb = g_callbacks; *cb; ++cb) {
        (*cb)(g_image.base, reason, nullptr);
    }
}

}  // namespace xb
