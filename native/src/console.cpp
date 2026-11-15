// A virtual console for the loaded program.
//
// Claude Code's interactive interface only starts when stdin and stdout are a
// console. Xbox apps have no console, so the loader makes three event objects,
// hands them to the program as its std handles, and answers every console call
// made on them. Output goes to the app's terminal; keystrokes from the app become
// console input records.
//
// The handles are real event objects because libuv waits on stdin to learn when
// input is ready: the input event is signaled whenever input is queued.
#include "xb.h"

#include <deque>
#include <stdio.h>
#include <string.h>
#include <vector>

namespace xb {

namespace {

enum Kind { kNone = 0, kInput = 1, kOutput = 2 };

struct Known {
    HANDLE handle;
    Kind kind;
};

CRITICAL_SECTION g_lock;
bool g_lock_ready = false;
bool g_enabled = false;
HANDLE g_in = nullptr;
HANDLE g_out = nullptr;
HANDLE g_err = nullptr;
std::vector<Known> g_known;
std::deque<INPUT_RECORD> g_queue;
std::wstring g_line;           // line being typed in cooked mode
std::wstring g_ready;          // characters ready to be read
std::string g_ready_bytes;     // UTF-8 left over from a short ReadFile
DWORD g_in_mode = ENABLE_PROCESSED_INPUT | ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT | ENABLE_INSERT_MODE | ENABLE_EXTENDED_FLAGS;
DWORD g_out_mode = ENABLE_PROCESSED_OUTPUT | ENABLE_WRAP_AT_EOL_OUTPUT | ENABLE_VIRTUAL_TERMINAL_PROCESSING;
SHORT g_cols = 120;
SHORT g_rows = 40;
std::wstring g_title = L"Claude Code";

void ensure_lock() {
    if (!g_lock_ready) {
        InitializeCriticalSection(&g_lock);
        g_lock_ready = true;
    }
}

struct Lock {
    Lock() { EnterCriticalSection(&g_lock); }
    ~Lock() { LeaveCriticalSection(&g_lock); }
};

Kind kind_of(HANDLE h) {
    if (!g_enabled || !h || h == INVALID_HANDLE_VALUE) return kNone;
    Lock lock;
    for (const Known& k : g_known) {
        if (k.handle == h) return k.kind;
    }
    return kNone;
}

void forget(HANDLE h) {
    Lock lock;
    for (size_t i = 0; i < g_known.size(); ++i) {
        if (g_known[i].handle == h) {
            g_known.erase(g_known.begin() + static_cast<ptrdiff_t>(i));
            return;
        }
    }
}

void write_wide(const wchar_t* s, size_t n) {
    if (!n) return;
    int bytes = WideCharToMultiByte(CP_UTF8, 0, s, static_cast<int>(n), nullptr, 0, nullptr, nullptr);
    if (bytes <= 0) return;
    std::string utf8(static_cast<size_t>(bytes), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s, static_cast<int>(n), &utf8[0], bytes, nullptr, nullptr);
    output_write(utf8.data(), utf8.size());
}

WORD virtual_key_for(wchar_t c, DWORD* state) {
    *state = 0;
    if (c == L'\r') return VK_RETURN;
    if (c == 8) return VK_BACK;
    if (c == L'\t') return VK_TAB;
    if (c == 27) return VK_ESCAPE;
    if (c == L' ') return VK_SPACE;
    if (c >= L'a' && c <= L'z') return static_cast<WORD>(c - L'a' + 'A');
    if (c >= L'A' && c <= L'Z') {
        *state = SHIFT_PRESSED;
        return static_cast<WORD>(c);
    }
    if (c >= L'0' && c <= L'9') return static_cast<WORD>(c);
    if (c >= 1 && c <= 26) {
        *state = LEFT_CTRL_PRESSED;
        return static_cast<WORD>('A' + c - 1);
    }
    return 0;
}

INPUT_RECORD key_record(wchar_t c) {
    INPUT_RECORD r{};
    r.EventType = KEY_EVENT;
    KEY_EVENT_RECORD& k = r.Event.KeyEvent;
    k.bKeyDown = TRUE;
    k.wRepeatCount = 1;
    k.uChar.UnicodeChar = c;
    k.wVirtualKeyCode = virtual_key_for(c, &k.dwControlKeyState);
    return r;
}

// Moves queued key presses into g_ready, editing a line first in cooked mode.
// Called with the lock held. Returns true when characters are ready.
bool cook_input() {
    bool line_mode = (g_in_mode & ENABLE_LINE_INPUT) != 0;
    bool echo = (g_in_mode & ENABLE_ECHO_INPUT) != 0;
    while (!g_queue.empty()) {
        INPUT_RECORD r = g_queue.front();
        g_queue.pop_front();
        if (r.EventType != KEY_EVENT || !r.Event.KeyEvent.bKeyDown) continue;
        wchar_t c = r.Event.KeyEvent.uChar.UnicodeChar;
        if (!c) continue;
        if (!line_mode) {
            g_ready.push_back(c);
            continue;
        }
        if (c == 8 || c == 127) {
            if (!g_line.empty()) {
                g_line.pop_back();
                if (echo) write_wide(L"\b \b", 3);
            }
            continue;
        }
        if (c == L'\r' || c == L'\n') {
            g_line += L"\r\n";
            if (echo) write_wide(L"\r\n", 2);
            g_ready += g_line;
            g_line.clear();
            break;
        }
        g_line.push_back(c);
        if (echo) write_wide(&c, 1);
    }
    if (g_queue.empty()) ResetEvent(g_in);
    return !g_ready.empty();
}

// Blocks until at least one character is ready, then copies up to cap of them.
DWORD read_chars(wchar_t* buf, DWORD cap) {
    for (;;) {
        {
            Lock lock;
            if (g_ready.empty()) cook_input();
            if (!g_ready.empty()) {
                DWORD n = static_cast<DWORD>(g_ready.size() < cap ? g_ready.size() : cap);
                memcpy(buf, g_ready.data(), n * sizeof(wchar_t));
                g_ready.erase(0, n);
                return n;
            }
        }
        WaitForSingleObject(g_in, INFINITE);
    }
}

// ---- Replacements ------------------------------------------------------------

DWORD WINAPI c_GetFileType(HANDLE h) {
    if (kind_of(h)) return FILE_TYPE_CHAR;
    return GetFileType(h);
}

BOOL WINAPI c_GetConsoleMode(HANDLE h, LPDWORD mode) {
    Kind k = kind_of(h);
    if (!k) return GetConsoleMode(h, mode);
    if (!mode) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    *mode = k == kInput ? g_in_mode : g_out_mode;
    return TRUE;
}

// Each kind of console event logs at most this many times.
bool log_once_more(volatile LONG& counter, LONG limit = 20) { return InterlockedIncrement(&counter) <= limit; }

BOOL WINAPI c_SetConsoleMode(HANDLE h, DWORD mode) {
    Kind k = kind_of(h);
    if (!k) return SetConsoleMode(h, mode);
    static volatile LONG n = 0;
    if (log_once_more(n)) log("console: %s mode set to 0x%lx", k == kInput ? "input" : "output", mode);
    Lock lock;
    if (k == kInput) g_in_mode = mode;
    else g_out_mode = mode;
    return TRUE;
}

BOOL WINAPI c_GetConsoleScreenBufferInfo(HANDLE h, PCONSOLE_SCREEN_BUFFER_INFO info) {
    if (kind_of(h) != kOutput) return GetConsoleScreenBufferInfo(h, info);
    if (!info) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    Lock lock;
    memset(info, 0, sizeof *info);
    info->dwSize.X = g_cols;
    info->dwSize.Y = g_rows;
    info->dwCursorPosition.X = 0;
    info->dwCursorPosition.Y = static_cast<SHORT>(g_rows - 1);
    info->wAttributes = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE;
    info->srWindow.Left = 0;
    info->srWindow.Top = 0;
    info->srWindow.Right = static_cast<SHORT>(g_cols - 1);
    info->srWindow.Bottom = static_cast<SHORT>(g_rows - 1);
    info->dwMaximumWindowSize = info->dwSize;
    return TRUE;
}

BOOL WINAPI c_GetConsoleCursorInfo(HANDLE h, PCONSOLE_CURSOR_INFO info) {
    if (kind_of(h) != kOutput) return GetConsoleCursorInfo(h, info);
    info->dwSize = 25;
    info->bVisible = TRUE;
    return TRUE;
}

BOOL WINAPI c_SetConsoleCursorInfo(HANDLE h, const CONSOLE_CURSOR_INFO* info) {
    if (kind_of(h) != kOutput) return SetConsoleCursorInfo(h, info);
    return TRUE;
}

BOOL WINAPI c_SetConsoleCursorPosition(HANDLE h, COORD pos) {
    if (kind_of(h) != kOutput) return SetConsoleCursorPosition(h, pos);
    return TRUE;
}

BOOL WINAPI c_SetConsoleTextAttribute(HANDLE h, WORD attr) {
    if (kind_of(h) != kOutput) return SetConsoleTextAttribute(h, attr);
    return TRUE;
}

BOOL WINAPI c_FillConsoleOutputCharacterW(HANDLE h, WCHAR c, DWORD len, COORD at, LPDWORD written) {
    if (kind_of(h) != kOutput) return FillConsoleOutputCharacterW(h, c, len, at, written);
    if (written) *written = len;
    return TRUE;
}

BOOL WINAPI c_FillConsoleOutputAttribute(HANDLE h, WORD attr, DWORD len, COORD at, LPDWORD written) {
    if (kind_of(h) != kOutput) return FillConsoleOutputAttribute(h, attr, len, at, written);
    if (written) *written = len;
    return TRUE;
}

BOOL WINAPI c_WriteConsoleW(HANDLE h, const void* buf, DWORD n, LPDWORD written, LPVOID reserved) {
    if (kind_of(h) != kOutput) return WriteConsoleW(h, buf, n, written, reserved);
    write_wide(static_cast<const wchar_t*>(buf), n);
    if (written) *written = n;
    return TRUE;
}

BOOL WINAPI c_WriteConsoleA(HANDLE h, const void* buf, DWORD n, LPDWORD written, LPVOID reserved) {
    if (kind_of(h) != kOutput) return WriteConsoleA(h, buf, n, written, reserved);
    output_write(static_cast<const char*>(buf), n);
    if (written) *written = n;
    return TRUE;
}

BOOL WINAPI c_WriteFile(HANDLE h, LPCVOID buf, DWORD n, LPDWORD written, LPOVERLAPPED ov) {
    Kind k = kind_of(h);
    if (k == kOutput) {
        output_write(static_cast<const char*>(buf), n);
        if (written) *written = n;
        return TRUE;
    }
    if (k == kInput) {
        SetLastError(ERROR_ACCESS_DENIED);
        return FALSE;
    }
    return WriteFile(h, buf, n, written, ov);
}

BOOL WINAPI c_ReadConsoleW(HANDLE h, LPVOID buf, DWORD n, LPDWORD got, PCONSOLE_READCONSOLE_CONTROL control) {
    if (kind_of(h) != kInput) return ReadConsoleW(h, buf, n, got, control);
    static volatile LONG calls = 0;
    bool show = log_once_more(calls);
    if (show) log("console: ReadConsoleW(%lu chars) waiting, mode 0x%lx", n, g_in_mode);
    DWORD count = n ? read_chars(static_cast<wchar_t*>(buf), n) : 0;
    if (show) log("console: ReadConsoleW -> %lu chars", count);
    if (got) *got = count;
    return TRUE;
}

// Reads UTF-8 for ReadFile and NtReadFile on the console input.
DWORD read_utf8(void* buf, DWORD n) {
    if (!n) return 0;
    {
        Lock lock;
        if (!g_ready_bytes.empty()) {
            DWORD c = static_cast<DWORD>(g_ready_bytes.size() < n ? g_ready_bytes.size() : n);
            memcpy(buf, g_ready_bytes.data(), c);
            g_ready_bytes.erase(0, c);
            return c;
        }
    }
    wchar_t wide[1024];
    DWORD chars = read_chars(wide, ARRAYSIZE(wide));
    std::string utf8 = narrow(std::wstring(wide, chars).c_str(), CP_UTF8);
    DWORD c = static_cast<DWORD>(utf8.size() < n ? utf8.size() : n);
    memcpy(buf, utf8.data(), c);
    if (utf8.size() > c) {
        Lock lock;
        g_ready_bytes.append(utf8, c, std::string::npos);
    }
    return c;
}

struct IoStatus {
    union {
        LONG status;
        PVOID pointer;
    };
    ULONG_PTR information;
};

using NtReadFile_fn = LONG(NTAPI*)(HANDLE, HANDLE, PVOID, PVOID, IoStatus*, PVOID, ULONG, PLARGE_INTEGER, PULONG);
using NtWriteFile_fn = LONG(NTAPI*)(HANDLE, HANDLE, PVOID, PVOID, IoStatus*, const void*, ULONG, PLARGE_INTEGER, PULONG);

LONG NTAPI c_NtReadFile(HANDLE h, HANDLE event, PVOID apc, PVOID apc_ctx, IoStatus* io, PVOID buf, ULONG n,
                        PLARGE_INTEGER offset, PULONG key) {
    Kind k = kind_of(h);
    if (k == kInput) {
        static volatile LONG calls = 0;
        bool show = log_once_more(calls);
        if (show) log("console: NtReadFile(%lu bytes) waiting", n);
        DWORD c = read_utf8(buf, n);
        if (show) log("console: NtReadFile -> %lu bytes", c);
        io->status = 0;
        io->information = c;
        if (event) SetEvent(event);
        return 0;
    }
    static auto real = reinterpret_cast<NtReadFile_fn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtReadFile"));
    return real ? real(h, event, apc, apc_ctx, io, buf, n, offset, key) : static_cast<LONG>(0xC0000002);
}

LONG NTAPI c_NtWriteFile(HANDLE h, HANDLE event, PVOID apc, PVOID apc_ctx, IoStatus* io, const void* buf, ULONG n,
                         PLARGE_INTEGER offset, PULONG key) {
    if (kind_of(h) == kOutput) {
        output_write(static_cast<const char*>(buf), n);
        io->status = 0;
        io->information = n;
        if (event) SetEvent(event);
        return 0;
    }
    static auto real = reinterpret_cast<NtWriteFile_fn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtWriteFile"));
    return real ? real(h, event, apc, apc_ctx, io, buf, n, offset, key) : static_cast<LONG>(0xC0000002);
}

BOOL WINAPI c_ReadFile(HANDLE h, LPVOID buf, DWORD n, LPDWORD got, LPOVERLAPPED ov) {
    Kind k = kind_of(h);
    if (k == kOutput) {
        SetLastError(ERROR_ACCESS_DENIED);
        return FALSE;
    }
    if (k != kInput) return ReadFile(h, buf, n, got, ov);
    static volatile LONG calls = 0;
    if (log_once_more(calls)) log("console: ReadFile(%lu bytes)%s", n, ov ? " overlapped" : "");
    if (!n) {
        if (got) *got = 0;
        return TRUE;
    }
    {
        Lock lock;
        if (g_ready_bytes.empty()) {
            // Nothing left over: fall through and read fresh characters below.
        } else {
            DWORD c = static_cast<DWORD>(g_ready_bytes.size() < n ? g_ready_bytes.size() : n);
            memcpy(buf, g_ready_bytes.data(), c);
            g_ready_bytes.erase(0, c);
            if (got) *got = c;
            return TRUE;
        }
    }
    wchar_t wide[1024];
    DWORD chars = read_chars(wide, ARRAYSIZE(wide));
    std::string utf8 = narrow(std::wstring(wide, chars).c_str(), CP_UTF8);
    DWORD c = static_cast<DWORD>(utf8.size() < n ? utf8.size() : n);
    memcpy(buf, utf8.data(), c);
    if (utf8.size() > c) {
        Lock lock;
        g_ready_bytes.append(utf8, c, std::string::npos);
    }
    if (got) *got = c;
    return TRUE;
}

BOOL WINAPI c_ReadConsoleInputW(HANDLE h, PINPUT_RECORD buf, DWORD n, LPDWORD got) {
    if (kind_of(h) != kInput) return ReadConsoleInputW(h, buf, n, got);
    static volatile LONG calls = 0;
    bool show = log_once_more(calls);
    for (;;) {
        {
            Lock lock;
            if (!g_queue.empty() || !n) {
                DWORD c = 0;
                while (c < n && !g_queue.empty()) {
                    buf[c++] = g_queue.front();
                    g_queue.pop_front();
                }
                if (g_queue.empty()) ResetEvent(g_in);
                if (got) *got = c;
                if (show) log("console: ReadConsoleInputW -> %lu records", c);
                return TRUE;
            }
        }
        WaitForSingleObject(g_in, INFINITE);
    }
}

BOOL WINAPI c_PeekConsoleInputW(HANDLE h, PINPUT_RECORD buf, DWORD n, LPDWORD got) {
    if (kind_of(h) != kInput) return PeekConsoleInputW(h, buf, n, got);
    Lock lock;
    DWORD c = 0;
    for (const INPUT_RECORD& r : g_queue) {
        if (c >= n) break;
        buf[c++] = r;
    }
    if (got) *got = c;
    return TRUE;
}

BOOL WINAPI c_GetNumberOfConsoleInputEvents(HANDLE h, LPDWORD count) {
    if (kind_of(h) != kInput) return GetNumberOfConsoleInputEvents(h, count);
    Lock lock;
    if (count) *count = static_cast<DWORD>(g_queue.size());
    return TRUE;
}

BOOL WINAPI c_FlushConsoleInputBuffer(HANDLE h) {
    if (kind_of(h) != kInput) return FlushConsoleInputBuffer(h);
    static volatile LONG calls = 0;
    if (log_once_more(calls)) log("console: input buffer flushed (%zu records dropped)", g_queue.size());
    Lock lock;
    g_queue.clear();
    ResetEvent(g_in);
    return TRUE;
}

BOOL WINAPI c_WriteConsoleInputW(HANDLE h, const INPUT_RECORD* buf, DWORD n, LPDWORD written) {
    if (kind_of(h) != kInput) return WriteConsoleInputW(h, buf, n, written);
    Lock lock;
    for (DWORD i = 0; i < n; ++i) g_queue.push_back(buf[i]);
    if (n) SetEvent(g_in);
    if (written) *written = n;
    return TRUE;
}

BOOL WINAPI c_CloseHandle(HANDLE h) {
    if (kind_of(h)) forget(h);
    return CloseHandle(h);
}

BOOL WINAPI c_DuplicateHandle(HANDLE src_process, HANDLE src, HANDLE dst_process, LPHANDLE dst, DWORD access,
                              BOOL inherit, DWORD options) {
    Kind k = kind_of(src);
    BOOL ok = DuplicateHandle(src_process, src, dst_process, dst, access, inherit, options);
    if (ok && k && dst && *dst && dst_process == GetCurrentProcess()) {
        Lock lock;
        g_known.push_back({*dst, k});
    }
    if (ok && k && (options & DUPLICATE_CLOSE_SOURCE)) forget(src);
    return ok;
}

UINT WINAPI c_GetConsoleCP() { return CP_UTF8; }
UINT WINAPI c_GetConsoleOutputCP() { return CP_UTF8; }
BOOL WINAPI c_SetConsoleCP(UINT) { return TRUE; }
BOOL WINAPI c_SetConsoleOutputCP(UINT) { return TRUE; }
BOOL WINAPI c_SetConsoleCtrlHandler(PHANDLER_ROUTINE, BOOL) { return TRUE; }
BOOL WINAPI c_GenerateConsoleCtrlEvent(DWORD, DWORD) { return TRUE; }

DWORD WINAPI c_GetConsoleTitleW(LPWSTR buf, DWORD size) {
    Lock lock;
    if (!buf || !size) return static_cast<DWORD>(g_title.size());
    DWORD n = static_cast<DWORD>(g_title.size() < size - 1 ? g_title.size() : size - 1);
    memcpy(buf, g_title.c_str(), n * sizeof(wchar_t));
    buf[n] = 0;
    return n;
}

BOOL WINAPI c_SetConsoleTitleW(LPCWSTR title) {
    Lock lock;
    g_title = title ? title : L"";
    return TRUE;
}

struct Override {
    const char* name;
    void* fn;
};

#define CONSOLE(n) {#n, reinterpret_cast<void*>(&c_##n)}
const Override kConsoleOverrides[] = {
    CONSOLE(GetFileType),
    CONSOLE(GetConsoleMode),
    CONSOLE(SetConsoleMode),
    CONSOLE(GetConsoleScreenBufferInfo),
    CONSOLE(GetConsoleCursorInfo),
    CONSOLE(SetConsoleCursorInfo),
    CONSOLE(SetConsoleCursorPosition),
    CONSOLE(SetConsoleTextAttribute),
    CONSOLE(FillConsoleOutputCharacterW),
    CONSOLE(FillConsoleOutputAttribute),
    CONSOLE(WriteConsoleW),
    CONSOLE(WriteConsoleA),
    CONSOLE(WriteFile),
    CONSOLE(ReadConsoleW),
    CONSOLE(ReadFile),
    CONSOLE(ReadConsoleInputW),
    CONSOLE(PeekConsoleInputW),
    CONSOLE(GetNumberOfConsoleInputEvents),
    CONSOLE(FlushConsoleInputBuffer),
    CONSOLE(WriteConsoleInputW),
    CONSOLE(CloseHandle),
    CONSOLE(DuplicateHandle),
    CONSOLE(GetConsoleCP),
    CONSOLE(GetConsoleOutputCP),
    CONSOLE(SetConsoleCP),
    CONSOLE(SetConsoleOutputCP),
    CONSOLE(SetConsoleCtrlHandler),
    CONSOLE(GenerateConsoleCtrlEvent),
    CONSOLE(GetConsoleTitleW),
    CONSOLE(SetConsoleTitleW),
    CONSOLE(NtReadFile),
    CONSOLE(NtWriteFile),
};
#undef CONSOLE

}  // namespace

bool console_init() {
    ensure_lock();
    g_in = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_out = CreateEventW(nullptr, TRUE, TRUE, nullptr);
    g_err = CreateEventW(nullptr, TRUE, TRUE, nullptr);
    if (!g_in || !g_out || !g_err) return false;
    {
        Lock lock;
        g_known.push_back({g_in, kInput});
        g_known.push_back({g_out, kOutput});
        g_known.push_back({g_err, kOutput});
    }
    g_enabled = true;
    log("virtual console ready, %d x %d", g_cols, g_rows);
    return true;
}

HANDLE console_stdin() { return g_in; }
HANDLE console_stdout() { return g_out; }
HANDLE console_stderr() { return g_err; }

void console_push_text(const char* utf8, size_t n) {
    ensure_lock();
    if (!n) return;
    std::wstring wide = widen(std::string(utf8, n).c_str(), CP_UTF8);
    static volatile LONG calls = 0;
    if (log_once_more(calls, 40)) {
        std::string shown;
        for (size_t i = 0; i < n && i < 24; ++i) {
            unsigned char c = static_cast<unsigned char>(utf8[i]);
            char part[8];
            if (c < 0x20 || c >= 0x7f) snprintf(part, sizeof part, "\\x%02x", c);
            else snprintf(part, sizeof part, "%c", c);
            shown += part;
        }
        log("console: input \"%s\" queued", shown.c_str());
    }
    Lock lock;
    for (wchar_t c : wide) g_queue.push_back(key_record(c));
    if (g_enabled && !wide.empty()) SetEvent(g_in);
}

void console_resize(SHORT cols, SHORT rows) {
    ensure_lock();
    if (cols < 10 || rows < 3) return;
    Lock lock;
    if (cols == g_cols && rows == g_rows) return;
    g_cols = cols;
    g_rows = rows;
    if (!g_enabled) return;
    INPUT_RECORD r{};
    r.EventType = WINDOW_BUFFER_SIZE_EVENT;
    r.Event.WindowBufferSizeEvent.dwSize.X = cols;
    r.Event.WindowBufferSizeEvent.dwSize.Y = rows;
    g_queue.push_back(r);
    SetEvent(g_in);
}

void* console_override(const char* name) {
    for (const Override& o : kConsoleOverrides) {
        if (strcmp(o.name, name) == 0) return o.fn;
    }
    return nullptr;
}

}  // namespace xb
