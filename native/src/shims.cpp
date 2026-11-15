// Replacements for Windows functions that must behave differently for a program
// loaded by hand inside an Xbox app. These win over the real functions even when
// the Xbox has them.
#include "xb.h"

#include <string.h>

namespace xb {

LoadLibraryExW_fn real_LoadLibraryExW = nullptr;

namespace {

using RtlPcToFileHeader_fn = PVOID(NTAPI*)(PVOID, PVOID*);
RtlPcToFileHeader_fn real_RtlPcToFileHeader = nullptr;

const wchar_t* file_part(const wchar_t* p) {
    const wchar_t* out = p;
    for (const wchar_t* c = p; *c; ++c) {
        if (*c == L'\\' || *c == L'/') out = c + 1;
    }
    return out;
}

bool is_guest_name(const wchar_t* name) {
    return _wcsicmp(file_part(name), file_part(g_image.path.c_str())) == 0;
}

std::wstring env(const wchar_t* name) {
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetEnvironmentVariableW(name, buf, ARRAYSIZE(buf));
    return (n && n < ARRAYSIZE(buf)) ? std::wstring(buf, n) : std::wstring();
}

// Copies s into a caller buffer the way GetModuleFileName does.
DWORD copy_out(const std::wstring& s, LPWSTR buf, DWORD size) {
    if (!size) {
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return 0;
    }
    if (s.size() + 1 > size) {
        memcpy(buf, s.c_str(), (size - 1) * sizeof(wchar_t));
        buf[size - 1] = 0;
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return size;
    }
    memcpy(buf, s.c_str(), (s.size() + 1) * sizeof(wchar_t));
    return static_cast<DWORD>(s.size());
}

// ---- Modules -------------------------------------------------------------

HMODULE WINAPI s_GetModuleHandleW(LPCWSTR name) {
    if (!name || is_guest_name(name)) return reinterpret_cast<HMODULE>(g_image.base);
    return GetModuleHandleW(name);
}

HMODULE WINAPI s_GetModuleHandleA(LPCSTR name) {
    if (!name) return reinterpret_cast<HMODULE>(g_image.base);
    return s_GetModuleHandleW(widen(name).c_str());
}

BOOL WINAPI s_GetModuleHandleExW(DWORD flags, LPCWSTR name, HMODULE* out) {
    if (!out) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    bool guest = (flags & GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS) ? in_guest(name) : (!name || is_guest_name(name));
    if (guest) {
        *out = reinterpret_cast<HMODULE>(g_image.base);
        return TRUE;
    }
    return GetModuleHandleExW(flags, name, out);
}

DWORD WINAPI s_GetModuleFileNameW(HMODULE h, LPWSTR buf, DWORD size) {
    if (!h || h == reinterpret_cast<HMODULE>(g_image.base)) return copy_out(g_image.path, buf, size);
    return GetModuleFileNameW(h, buf, size);
}

DWORD WINAPI s_GetModuleFileNameA(HMODULE h, LPSTR buf, DWORD size) {
    if (h && h != reinterpret_cast<HMODULE>(g_image.base)) return GetModuleFileNameA(h, buf, size);
    std::string s = narrow(g_image.path.c_str(), CP_ACP);
    if (!size) {
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return 0;
    }
    if (s.size() + 1 > size) {
        memcpy(buf, s.c_str(), size - 1);
        buf[size - 1] = 0;
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return size;
    }
    memcpy(buf, s.c_str(), s.size() + 1);
    return static_cast<DWORD>(s.size());
}

FARPROC WINAPI s_GetProcAddress(HMODULE h, LPCSTR name) {
    if (!h || h == reinterpret_cast<HMODULE>(g_image.base)) {
        void* p = IS_INTRESOURCE(name)
                      ? find_export(g_image, nullptr, static_cast<uint16_t>(reinterpret_cast<uintptr_t>(name)))
                      : find_export(g_image, name, 0);
        if (!p) SetLastError(ERROR_PROC_NOT_FOUND);
        return reinterpret_cast<FARPROC>(p);
    }
    // Functions looked up at run time get the same replacements as imported ones.
    if (!IS_INTRESOURCE(name)) {
        if (void* o = lookup_override(name)) return reinterpret_cast<FARPROC>(o);
    }
    return GetProcAddress(h, name);
}

HMODULE load_library(LPCWSTR name, DWORD flags) {
    if (!name) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return nullptr;
    }
    if (is_guest_name(name)) return reinterpret_cast<HMODULE>(g_image.base);
    if (HMODULE h = GetModuleHandleW(name)) return h;
    if (real_LoadLibraryExW) {
        if (HMODULE h = real_LoadLibraryExW(name, nullptr, flags)) return h;
    }
    HMODULE h = LoadPackagedLibrary(name, 0);
    if (!h) log("LoadLibrary(%s) failed: error %lu", narrow(name).c_str(), GetLastError());
    return h;
}

HMODULE WINAPI s_LoadLibraryExW(LPCWSTR name, HANDLE, DWORD flags) { return load_library(name, flags); }
HMODULE WINAPI s_LoadLibraryW(LPCWSTR name) { return load_library(name, 0); }
HMODULE WINAPI s_LoadLibraryExA(LPCSTR name, HANDLE, DWORD flags) {
    return name ? load_library(widen(name).c_str(), flags) : load_library(nullptr, 0);
}
HMODULE WINAPI s_LoadLibraryA(LPCSTR name) { return s_LoadLibraryExA(name, nullptr, 0); }

BOOL WINAPI s_FreeLibrary(HMODULE h) {
    if (h == reinterpret_cast<HMODULE>(g_image.base)) return TRUE;
    return FreeLibrary(h);
}

// Every program thread ends through here, so Windows gets its TLS entry back first.
void end_program_thread() {
    tls_run_callbacks(DLL_THREAD_DETACH);
    tls_thread_exit();
}

VOID WINAPI s_ExitThread(DWORD code) {
    end_program_thread();
    ExitThread(code);
}

VOID WINAPI s_FreeLibraryAndExitThread(HMODULE h, DWORD code) {
    if (h != reinterpret_cast<HMODULE>(g_image.base)) FreeLibrary(h);
    end_program_thread();
    ExitThread(code);
}

PVOID NTAPI s_RtlPcToFileHeader(PVOID pc, PVOID* base) {
    if (in_guest(pc)) {
        *base = g_image.base;
        return g_image.base;
    }
    if (real_RtlPcToFileHeader) return real_RtlPcToFileHeader(pc, base);
    *base = nullptr;
    return nullptr;
}

// ---- Process ---------------------------------------------------------------

LPWSTR WINAPI s_GetCommandLineW() { return const_cast<LPWSTR>(command_line_w().c_str()); }
LPSTR WINAPI s_GetCommandLineA() { return const_cast<LPSTR>(command_line_a().c_str()); }

VOID WINAPI s_ExitProcess(UINT code) { process_exit(code); }
VOID NTAPI s_RtlExitUserProcess(LONG status) { process_exit(static_cast<UINT>(status)); }

BOOL WINAPI s_TerminateProcess(HANDLE h, UINT code) {
    if (h == GetCurrentProcess() || GetProcessId(h) == GetCurrentProcessId()) process_exit(code);
    return TerminateProcess(h, code);
}

// ---- Threads ---------------------------------------------------------------
// Every thread that runs claude.exe code needs its TLS block before it starts.

struct ThreadStart {
    LPTHREAD_START_ROUTINE fn;
    void* arg;
};

DWORD WINAPI thread_trampoline(void* p) {
    ThreadStart s = *static_cast<ThreadStart*>(p);
    HeapFree(GetProcessHeap(), 0, p);
    tls_ensure_thread();
    tls_run_callbacks(DLL_THREAD_ATTACH);
    DWORD r = s.fn(s.arg);
    end_program_thread();
    return r;
}

HANDLE WINAPI s_CreateThread(LPSECURITY_ATTRIBUTES sa, SIZE_T stack, LPTHREAD_START_ROUTINE fn, LPVOID arg,
                             DWORD flags, LPDWORD id) {
    auto* s = static_cast<ThreadStart*>(HeapAlloc(GetProcessHeap(), 0, sizeof(ThreadStart)));
    if (!s) {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return nullptr;
    }
    s->fn = fn;
    s->arg = arg;
    HANDLE h = CreateThread(sa, stack, thread_trampoline, s, flags, id);
    if (h) register_guest_thread(h);
    else HeapFree(GetProcessHeap(), 0, s);
    return h;
}

DWORD WINAPI work_trampoline(void* p) {
    ThreadStart s = *static_cast<ThreadStart*>(p);
    HeapFree(GetProcessHeap(), 0, p);
    void* saved = tls_enter();
    DWORD r = s.fn(s.arg);
    tls_leave(saved);
    return r;
}

BOOL WINAPI s_QueueUserWorkItem(LPTHREAD_START_ROUTINE fn, PVOID ctx, ULONG flags) {
    auto* s = static_cast<ThreadStart*>(HeapAlloc(GetProcessHeap(), 0, sizeof(ThreadStart)));
    if (!s) {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return FALSE;
    }
    s->fn = fn;
    s->arg = ctx;
    if (!QueueUserWorkItem(work_trampoline, s, flags)) {
        HeapFree(GetProcessHeap(), 0, s);
        return FALSE;
    }
    return TRUE;
}

struct WaitStart {
    WAITORTIMERCALLBACK fn;
    void* ctx;
    bool once;
};

VOID CALLBACK wait_trampoline(PVOID p, BOOLEAN fired) {
    auto* w = static_cast<WaitStart*>(p);
    WAITORTIMERCALLBACK fn = w->fn;
    void* ctx = w->ctx;
    if (w->once) HeapFree(GetProcessHeap(), 0, w);
    void* saved = tls_enter();
    fn(ctx, fired);
    tls_leave(saved);
}

BOOL WINAPI s_RegisterWaitForSingleObject(PHANDLE out, HANDLE h, WAITORTIMERCALLBACK fn, PVOID ctx, ULONG ms,
                                          ULONG flags) {
    auto* w = static_cast<WaitStart*>(HeapAlloc(GetProcessHeap(), 0, sizeof(WaitStart)));
    if (!w) {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return FALSE;
    }
    w->fn = fn;
    w->ctx = ctx;
    w->once = (flags & WT_EXECUTEONLYONCE) != 0;
    if (!RegisterWaitForSingleObject(out, h, wait_trampoline, w, ms, flags)) {
        HeapFree(GetProcessHeap(), 0, w);
        return FALSE;
    }
    return TRUE;
}

// ---- Exception handlers ------------------------------------------------------
// Windows runs these on whichever thread hits an exception, including the app's
// own threads, which have no TLS for claude.exe. After the program exits its
// threads are frozen, possibly holding locks, so its handlers must not run at all.

// The program's handlers only see exceptions in its own code or on its own threads.
// Its crash handler would otherwise end the program over a fault the app handles.
bool program_exception(EXCEPTION_POINTERS* ep) {
    return in_guest(ep->ExceptionRecord->ExceptionAddress) || tls_thread_owned();
}

LONG CALLBACK veh_trampoline(EXCEPTION_POINTERS* ep, PVECTORED_EXCEPTION_HANDLER handler) {
    if (!program_running() || !program_exception(ep)) return EXCEPTION_CONTINUE_SEARCH;
    void* saved = tls_enter();
    LONG r = handler(ep);
    tls_leave(saved);
    return r;
}

PVOID WINAPI s_AddVectoredExceptionHandler(ULONG first, PVECTORED_EXCEPTION_HANDLER handler) {
    void* thunk = make_bound_thunk(reinterpret_cast<void*>(handler), reinterpret_cast<void*>(&veh_trampoline));
    if (!thunk) {
        log("AddVectoredExceptionHandler: registering claude.exe's handler unwrapped");
        return AddVectoredExceptionHandler(first, handler);
    }
    return AddVectoredExceptionHandler(first, reinterpret_cast<PVECTORED_EXCEPTION_HANDLER>(thunk));
}

LPTOP_LEVEL_EXCEPTION_FILTER volatile g_guest_filter = nullptr;
LPTOP_LEVEL_EXCEPTION_FILTER g_host_filter = nullptr;
volatile LONG g_filter_installed = 0;

LONG WINAPI filter_trampoline(EXCEPTION_POINTERS* ep) {
    LPTOP_LEVEL_EXCEPTION_FILTER guest = g_guest_filter;
    if (guest && program_running() && program_exception(ep)) {
        void* saved = tls_enter();
        LONG r = guest(ep);
        tls_leave(saved);
        if (r != EXCEPTION_CONTINUE_SEARCH) return r;
    }
    return g_host_filter ? g_host_filter(ep) : EXCEPTION_CONTINUE_SEARCH;
}

LPTOP_LEVEL_EXCEPTION_FILTER WINAPI s_SetUnhandledExceptionFilter(LPTOP_LEVEL_EXCEPTION_FILTER filter) {
    LPTOP_LEVEL_EXCEPTION_FILTER previous = g_guest_filter;
    g_guest_filter = filter;
    if (InterlockedExchange(&g_filter_installed, 1) == 0) {
        g_host_filter = SetUnhandledExceptionFilter(filter_trampoline);
    }
    return previous;
}

// ---- Memory ----------------------------------------------------------------
// Xbox apps may only create executable memory through the *FromApp functions,
// and only by allocating it writable first and then changing its protection.

constexpr DWORD kExecMask = PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;

LPVOID WINAPI s_VirtualAlloc(LPVOID addr, SIZE_T size, DWORD type, DWORD prot) {
    if (!(prot & kExecMask)) return VirtualAllocFromApp(addr, size, type, prot);
    DWORD writable = (prot & ~0xFFu) | PAGE_READWRITE;
    void* p = VirtualAllocFromApp(addr, size, type, writable);
    if (p && (type & MEM_COMMIT)) {
        ULONG old = 0;
        if (!VirtualProtectFromApp(addr ? addr : p, size, prot, &old)) {
            static volatile LONG warned = 0;
            if (InterlockedIncrement(&warned) <= 5) {
                log("VirtualAlloc: could not make %zu bytes executable: error %lu", size, GetLastError());
            }
        }
    }
    return p;
}

BOOL WINAPI s_VirtualProtect(LPVOID addr, SIZE_T size, DWORD prot, PDWORD old) {
    ULONG prev = 0;
    BOOL ok = VirtualProtectFromApp(addr, size, prot, &prev);
    if (old) *old = prev;
    return ok;
}

// ---- User folders ----------------------------------------------------------
// The program's home folder is the app's own storage, set by the C# app in USERPROFILE.

const GUID kFolderProfile = {0x5E6C858F, 0x0E22, 0x4760, {0x9A, 0xFE, 0xEA, 0x33, 0x17, 0xB6, 0x71, 0x73}};
const GUID kFolderLocalAppData = {0xF1B32785, 0x6FBA, 0x4FCF, {0x9D, 0x55, 0x7B, 0x8E, 0x7F, 0x15, 0x70, 0x91}};
const GUID kFolderRoamingAppData = {0x3EB685DB, 0x65F9, 0x4CF6, {0xA0, 0x3A, 0xE3, 0xEF, 0x65, 0x72, 0x9F, 0x3D}};
const GUID kFolderLocalAppDataLow = {0xA520A1A4, 0x1780, 0x4FF6, {0xBD, 0x18, 0x16, 0x73, 0x43, 0xC5, 0xAF, 0x16}};

HRESULT WINAPI s_SHGetKnownFolderPath(const GUID& id, DWORD, HANDLE, PWSTR* out) {
    if (!out) return E_INVALIDARG;
    *out = nullptr;
    std::wstring home = env(L"USERPROFILE");
    std::wstring path;
    if (id == kFolderProfile) path = home;
    else if (id == kFolderLocalAppData) path = home + L"\\AppData\\Local";
    else if (id == kFolderRoamingAppData) path = home + L"\\AppData\\Roaming";
    else if (id == kFolderLocalAppDataLow) path = home + L"\\AppData\\LocalLow";
    if (home.empty() || path.empty()) return E_FAIL;
    size_t bytes = (path.size() + 1) * sizeof(wchar_t);
    *out = static_cast<PWSTR>(CoTaskMemAlloc(bytes));
    if (!*out) return E_OUTOFMEMORY;
    memcpy(*out, path.c_str(), bytes);
    return S_OK;
}

BOOL copy_with_size(const std::wstring& s, LPWSTR buf, LPDWORD size) {
    if (!size) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    DWORD need = static_cast<DWORD>(s.size() + 1);
    if (!buf || *size < need) {
        *size = need;
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return FALSE;
    }
    memcpy(buf, s.c_str(), need * sizeof(wchar_t));
    *size = need;
    return TRUE;
}

BOOL WINAPI s_GetUserProfileDirectoryW(HANDLE, LPWSTR buf, LPDWORD size) {
    return copy_with_size(env(L"USERPROFILE"), buf, size);
}

BOOL WINAPI s_GetUserNameW(LPWSTR buf, LPDWORD size) { return copy_with_size(L"xbox", buf, size); }

// Logs every program the loaded program tries to start and whether the Xbox allowed it.
BOOL WINAPI s_CreateProcessW(LPCWSTR app, LPWSTR cmd, LPSECURITY_ATTRIBUTES psa, LPSECURITY_ATTRIBUTES tsa, BOOL inherit,
                             DWORD flags, LPVOID env, LPCWSTR cwd, LPSTARTUPINFOW si, LPPROCESS_INFORMATION pi) {
    BOOL ok = CreateProcessW(app, cmd, psa, tsa, inherit, flags, env, cwd, si, pi);
    DWORD e = ok ? 0 : GetLastError();
    log("CreateProcessW(%s, %s) -> %s (error %lu)", app ? narrow(app).c_str() : "-",
        cmd ? narrow(cmd).substr(0, 300).c_str() : "-", ok ? "started" : "failed", e);
    if (!ok) SetLastError(e);
    return ok;
}

// There's no browser to hand links to. Reporting failure makes Claude Code show
// the sign-in link instead, which the app turns into a QR code.
BOOL WINAPI s_ShellExecuteExW(void*) {
    log("ShellExecuteExW: no browser here, so the program was told it failed");
    SetLastError(ERROR_NO_ASSOCIATION);
    return FALSE;
}

struct Override {
    const char* name;
    void* fn;
};

#define OVERRIDE(n) {#n, reinterpret_cast<void*>(&s_##n)}
const Override kOverrides[] = {
    OVERRIDE(GetModuleHandleW),
    OVERRIDE(GetModuleHandleA),
    OVERRIDE(GetModuleHandleExW),
    OVERRIDE(GetModuleFileNameW),
    OVERRIDE(GetModuleFileNameA),
    OVERRIDE(GetProcAddress),
    OVERRIDE(LoadLibraryW),
    OVERRIDE(LoadLibraryA),
    OVERRIDE(LoadLibraryExW),
    OVERRIDE(LoadLibraryExA),
    OVERRIDE(FreeLibrary),
    OVERRIDE(FreeLibraryAndExitThread),
    OVERRIDE(ExitThread),
    OVERRIDE(RtlPcToFileHeader),
    OVERRIDE(GetCommandLineW),
    OVERRIDE(GetCommandLineA),
    OVERRIDE(ExitProcess),
    OVERRIDE(RtlExitUserProcess),
    OVERRIDE(TerminateProcess),
    OVERRIDE(CreateThread),
    OVERRIDE(QueueUserWorkItem),
    OVERRIDE(RegisterWaitForSingleObject),
    OVERRIDE(AddVectoredExceptionHandler),
    OVERRIDE(SetUnhandledExceptionFilter),
    OVERRIDE(VirtualAlloc),
    OVERRIDE(VirtualProtect),
    OVERRIDE(SHGetKnownFolderPath),
    OVERRIDE(GetUserProfileDirectoryW),
    OVERRIDE(GetUserNameW),
    OVERRIDE(ShellExecuteExW),
    OVERRIDE(CreateProcessW),
};
#undef OVERRIDE

}  // namespace

void shims_init() {
    HMODULE kernelbase = GetModuleHandleW(L"kernelbase.dll");
    HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    FARPROC p = kernelbase ? GetProcAddress(kernelbase, "LoadLibraryExW") : nullptr;
    if (!p && kernel32) p = GetProcAddress(kernel32, "LoadLibraryExW");
    real_LoadLibraryExW = reinterpret_cast<LoadLibraryExW_fn>(p);
    real_RtlPcToFileHeader =
        reinterpret_cast<RtlPcToFileHeader_fn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlPcToFileHeader"));
    log("LoadLibraryExW: %s", real_LoadLibraryExW ? "available" : "missing, only app package DLLs can load");
}

void* lookup_override(const char* name) {
    if (void* c = console_override(name)) return c;
    if (void* n = net_override(name)) return n;
    for (const Override& o : kOverrides) {
        if (strcmp(o.name, name) == 0) return o.fn;
    }
    return nullptr;
}

}  // namespace xb
