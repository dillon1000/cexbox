// The exported API, logging, the program's stdin/stdout pipes and its lifetime.
#include "xb.h"
#include "xbloader.h"

#include <stdarg.h>
#include <stdio.h>
#include <vector>

namespace xb {

namespace {

CRITICAL_SECTION g_log_lock;
CRITICAL_SECTION g_out_lock;
CRITICAL_SECTION g_thread_lock;

HANDLE g_log_file = INVALID_HANDLE_VALUE;
HANDLE g_output_file = INVALID_HANDLE_VALUE;
ULONGLONG g_start_ticks = 0;

std::string g_output;  // program output not yet read by the app
constexpr size_t kOutputLimit = 8u << 20;

std::vector<HANDLE> g_threads;

volatile LONG g_state = XB_NOT_STARTED;
volatile LONG g_exit_code = 0;
std::wstring g_error;
std::wstring g_command_w;
std::string g_command_a;

HANDLE g_stdin_read = nullptr;
HANDLE g_stdin_write = nullptr;
HANDLE g_stdout_read = nullptr;
HANDLE volatile g_stdout_write = nullptr;

// RTL_USER_PROCESS_PARAMETERS field offsets on x64.
constexpr size_t kTebPeb = 0x60;
constexpr size_t kPebParams = 0x20;
constexpr size_t kParamsStdIn = 0x20;
constexpr size_t kParamsStdOut = 0x28;
constexpr size_t kParamsStdErr = 0x30;
constexpr size_t kParamsImagePath = 0x60;
constexpr size_t kParamsCommandLine = 0x70;

struct UnicodeString {
    USHORT length;
    USHORT max_length;
    PWSTR buffer;
};

HANDLE open_log(const std::wstring& dir, const wchar_t* name) {
    std::wstring path = dir + L"\\" + name;
    return CreateFile2(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, CREATE_ALWAYS, nullptr);
}

int fail(const std::string& message) {
    log("failed: %s", message.c_str());
    g_error = widen(message.c_str(), CP_UTF8);
    InterlockedExchange(&g_state, XB_FAILED);
    return -1;
}

void set_unicode_string(uint8_t* field, const std::wstring& s) {
    auto* u = reinterpret_cast<UnicodeString*>(field);
    u->length = static_cast<USHORT>(s.size() * sizeof(wchar_t));
    u->max_length = static_cast<USHORT>((s.size() + 1) * sizeof(wchar_t));
    u->buffer = const_cast<PWSTR>(s.c_str());
}

// Some code in claude.exe reads its own path, command line and std handles straight
// from the process parameters block instead of asking Windows, so point those at it.
void patch_process_parameters() {
    auto* peb = *reinterpret_cast<uint8_t**>(reinterpret_cast<uint8_t*>(NtCurrentTeb()) + kTebPeb);
    auto* params = *reinterpret_cast<uint8_t**>(peb + kPebParams);
    *reinterpret_cast<HANDLE*>(params + kParamsStdIn) = g_stdin_read;
    *reinterpret_cast<HANDLE*>(params + kParamsStdOut) = g_stdout_write;
    *reinterpret_cast<HANDLE*>(params + kParamsStdErr) = g_stdout_write;
    set_unicode_string(params + kParamsImagePath, g_image.path);
    set_unicode_string(params + kParamsCommandLine, g_command_w);
}

DWORD WINAPI output_reader(void*) {
    char buf[16384];
    DWORD n = 0;
    while (ReadFile(g_stdout_read, buf, sizeof buf, &n, nullptr) && n) {
        EnterCriticalSection(&g_out_lock);
        if (g_output.size() + n > kOutputLimit) g_output.erase(0, g_output.size() / 2);
        g_output.append(buf, n);
        LeaveCriticalSection(&g_out_lock);
        if (g_output_file != INVALID_HANDLE_VALUE) {
            DWORD written = 0;
            WriteFile(g_output_file, buf, n, &written, nullptr);
        }
    }
    log("program output closed");
    return 0;
}

LONG CALLBACK log_crashes(EXCEPTION_POINTERS* ep) {
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    bool serious = code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_ILLEGAL_INSTRUCTION ||
                   code == EXCEPTION_PRIV_INSTRUCTION || code == EXCEPTION_STACK_OVERFLOW ||
                   code == 0xC0000409 /* stack buffer overrun / fail fast */;
    if (serious) {
        static volatile LONG count = 0;
        if (InterlockedIncrement(&count) <= 20) {
            void* at = ep->ExceptionRecord->ExceptionAddress;
            ULONG_PTR* info = ep->ExceptionRecord->ExceptionInformation;
            if (in_guest(at)) {
                log("exception 0x%08lx at claude.exe+0x%llx (info %llx %llx)", code,
                    static_cast<unsigned long long>(static_cast<uint8_t*>(at) - g_image.base),
                    static_cast<unsigned long long>(info[0]), static_cast<unsigned long long>(info[1]));
            } else {
                log("exception 0x%08lx at %p outside claude.exe (info %llx %llx)", code, at,
                    static_cast<unsigned long long>(info[0]), static_cast<unsigned long long>(info[1]));
            }
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

DWORD WINAPI guest_main(void*) {
    tls_ensure_thread();
    tls_run_callbacks(DLL_PROCESS_ATTACH);
    log("starting claude.exe");
    using Entry = int(WINAPI*)(void*);
    void* peb = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(NtCurrentTeb()) + kTebPeb);
    int result = reinterpret_cast<Entry>(g_image.base + g_image.entry_rva)(peb);
    process_exit(static_cast<UINT>(result));
}

}  // namespace

void log(const char* fmt, ...) {
    char buf[2048];
    int n = snprintf(buf, sizeof buf, "[%9.3f] ", (GetTickCount64() - g_start_ticks) / 1000.0);
    if (n < 0) n = 0;
    va_list ap;
    va_start(ap, fmt);
    int m = vsnprintf(buf + n, sizeof buf - n - 2, fmt, ap);
    va_end(ap);
    size_t len = static_cast<size_t>(n) + (m > 0 ? static_cast<size_t>(m) : 0);
    if (len > sizeof buf - 2) len = sizeof buf - 2;
    buf[len++] = '\r';
    buf[len++] = '\n';
    EnterCriticalSection(&g_log_lock);
    if (g_log_file != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(g_log_file, buf, static_cast<DWORD>(len), &written, nullptr);
    }
    LeaveCriticalSection(&g_log_lock);
}

std::string narrow(const wchar_t* s, UINT code_page) {
    if (!s || !*s) return std::string();
    int n = WideCharToMultiByte(code_page, 0, s, -1, nullptr, 0, nullptr, nullptr);
    std::string out(n > 0 ? n - 1 : 0, '\0');
    if (n > 1) WideCharToMultiByte(code_page, 0, s, -1, &out[0], n, nullptr, nullptr);
    return out;
}

std::wstring widen(const char* s, UINT code_page) {
    if (!s || !*s) return std::wstring();
    int n = MultiByteToWideChar(code_page, 0, s, -1, nullptr, 0);
    std::wstring out(n > 0 ? n - 1 : 0, L'\0');
    if (n > 1) MultiByteToWideChar(code_page, 0, s, -1, &out[0], n);
    return out;
}

bool program_running() { return g_state == XB_RUNNING; }

const std::wstring& command_line_w() { return g_command_w; }
const std::string& command_line_a() { return g_command_a; }

void register_guest_thread(HANDLE thread) {
    HANDLE dup = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), thread, GetCurrentProcess(), &dup, 0, FALSE, DUPLICATE_SAME_ACCESS)) return;
    EnterCriticalSection(&g_thread_lock);
    g_threads.push_back(dup);
    LeaveCriticalSection(&g_thread_lock);
}

// Ends the program without ending the app: other program threads are frozen,
// its output pipe is closed, and the calling thread exits.
[[noreturn]] void process_exit(UINT code) {
    if (InterlockedCompareExchange(&g_state, XB_EXITED, XB_RUNNING) == XB_RUNNING) {
        InterlockedExchange(&g_exit_code, static_cast<LONG>(code));
        log("claude.exe exited with code %u", code);
        DWORD self = GetCurrentThreadId();
        EnterCriticalSection(&g_thread_lock);
        for (HANDLE h : g_threads) {
            if (GetThreadId(h) != self) SuspendThread(h);
        }
        LeaveCriticalSection(&g_thread_lock);
        HANDLE w = InterlockedExchangePointer(const_cast<HANDLE*>(&g_stdout_write), nullptr);
        if (w) CloseHandle(w);
    }
    ExitThread(code);
}

}  // namespace xb

using namespace xb;

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        InitializeCriticalSection(&g_log_lock);
        InitializeCriticalSection(&g_out_lock);
        InitializeCriticalSection(&g_thread_lock);
        g_start_ticks = GetTickCount64();
    }
    return TRUE;
}

extern "C" XB_API int __stdcall xb_setenv(const wchar_t* name, const wchar_t* value) {
    return SetEnvironmentVariableW(name, value) ? 0 : static_cast<int>(GetLastError());
}

extern "C" XB_API int __stdcall xb_start(const wchar_t* exe_path, const wchar_t* command_line,
                                         const wchar_t* work_dir, const wchar_t* log_dir) {
    if (InterlockedCompareExchange(&g_state, XB_STARTING, XB_NOT_STARTED) != XB_NOT_STARTED) {
        g_error = L"A program has already run in this app session. Restart the app to run it again.";
        return -1;
    }
    if (!exe_path || !command_line) return fail("Missing program path or command line");

    if (log_dir && *log_dir) {
        g_log_file = open_log(log_dir, L"loader.log");
        g_output_file = open_log(log_dir, L"output.log");
    }
    g_start_ticks = GetTickCount64();
    log("cexbox loader, program %s", narrow(exe_path).c_str());
    log("command line: %s", narrow(command_line).c_str());

    g_command_w = command_line;
    g_command_a = narrow(command_line, CP_ACP);
    if (work_dir && *work_dir && !SetCurrentDirectoryW(work_dir)) {
        log("could not change folder to %s: error %lu", narrow(work_dir).c_str(), GetLastError());
    }

    shims_init();

    HANDLE out_write = nullptr;
    if (!CreatePipe(&g_stdin_read, &g_stdin_write, nullptr, 0) ||
        !CreatePipe(&g_stdout_read, &out_write, nullptr, 1 << 16)) {
        return fail("Could not create the program's input and output pipes");
    }
    g_stdout_write = out_write;
    SetStdHandle(STD_INPUT_HANDLE, g_stdin_read);
    SetStdHandle(STD_OUTPUT_HANDLE, out_write);
    SetStdHandle(STD_ERROR_HANDLE, out_write);

    std::string err;
    if (!load_image(exe_path, g_image, err)) return fail(err);
    patch_process_parameters();

    AddVectoredExceptionHandler(1, log_crashes);

    HANDLE reader = CreateThread(nullptr, 0, output_reader, nullptr, 0, nullptr);
    if (reader) CloseHandle(reader);

    size_t stack = g_image.stack_reserve ? g_image.stack_reserve : (16u << 20);
    InterlockedExchange(&g_state, XB_RUNNING);
    HANDLE main_thread = CreateThread(nullptr, stack, guest_main, nullptr, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    if (!main_thread) return fail("Could not start the program's main thread");
    register_guest_thread(main_thread);
    CloseHandle(main_thread);
    log("main thread started with a %zu byte stack", stack);
    return 0;
}

extern "C" XB_API int __stdcall xb_read_output(uint8_t* buffer, int capacity) {
    if (!buffer || capacity <= 0) return 0;
    EnterCriticalSection(&g_out_lock);
    size_t n = g_output.size() < static_cast<size_t>(capacity) ? g_output.size() : static_cast<size_t>(capacity);
    memcpy(buffer, g_output.data(), n);
    g_output.erase(0, n);
    LeaveCriticalSection(&g_out_lock);
    return static_cast<int>(n);
}

extern "C" XB_API int __stdcall xb_write_input(const uint8_t* buffer, int length) {
    if (!g_stdin_write || !buffer || length < 0) return -1;
    DWORD written = 0;
    if (!WriteFile(g_stdin_write, buffer, static_cast<DWORD>(length), &written, nullptr)) return -1;
    return static_cast<int>(written);
}

extern "C" XB_API int __stdcall xb_poll(int* exit_code) {
    if (exit_code) *exit_code = g_exit_code;
    return g_state;
}

extern "C" XB_API int __stdcall xb_last_error(wchar_t* buffer, int capacity) {
    if (!buffer || capacity <= 0) return static_cast<int>(g_error.size());
    size_t n = g_error.size() < static_cast<size_t>(capacity - 1) ? g_error.size() : static_cast<size_t>(capacity - 1);
    memcpy(buffer, g_error.c_str(), n * sizeof(wchar_t));
    buffer[n] = 0;
    return static_cast<int>(n);
}
