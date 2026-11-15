// Shared declarations for the loader. Include this before any other Windows header.
#pragma once

// Declare the whole Win32 API, desktop parts included. The DLL links only against
// WindowsApp.lib, so calling anything an Xbox app can't import fails at link time
// instead of failing to load on the console.
// (Only MSVC needs this; mingw's headers already declare everything.)
#ifdef _MSC_VER
#ifdef WINAPI_FAMILY
#undef WINAPI_FAMILY
#endif
#define WINAPI_FAMILY 100 /* WINAPI_FAMILY_DESKTOP_APP */
#endif

#include <windows.h>
#include <objbase.h>
#include <stdint.h>
#include <string>

namespace xb {

// The program loaded into this process (claude.exe).
struct Image {
    uint8_t* base = nullptr;
    size_t size = 0;
    std::wstring path;
    IMAGE_NT_HEADERS64* nt = nullptr;
    uint32_t entry_rva = 0;
    size_t stack_reserve = 0;
};

extern Image g_image;

// host.cpp
void log(const char* fmt, ...);
void output_write(const char* data, size_t n);  // program output for the app to show
bool program_running();
[[noreturn]] void process_exit(UINT code);
void register_guest_thread(HANDLE thread);
const std::wstring& command_line_w();
const std::string& command_line_a();
std::string narrow(const wchar_t* s, UINT code_page = CP_UTF8);
std::wstring widen(const char* s, UINT code_page = CP_ACP);

// loader.cpp
bool load_image(const wchar_t* path, Image& img, std::string& err);
void* find_export(const Image& img, const char* name, uint16_t ordinal);
void* resolve_import(const char* dll, const char* name, uint16_t ordinal);
bool in_guest(const void* p);

// shims.cpp
void shims_init();
void* lookup_override(const char* name);

using LoadLibraryExW_fn = HMODULE(WINAPI*)(LPCWSTR, HANDLE, DWORD);
extern LoadLibraryExW_fn real_LoadLibraryExW;

// stubs.cpp
void* make_stub(const char* dll, const char* name, uint16_t ordinal);
bool stubs_finalize();
// Returns code that calls trampoline(first argument, target). Used to give a
// callback that takes one argument a second, hidden one.
void* make_bound_thunk(void* target, void* trampoline);

// console.cpp
bool console_init();
HANDLE console_stdin();
HANDLE console_stdout();
HANDLE console_stderr();
void console_push_text(const char* utf8, size_t n);
void console_resize(SHORT cols, SHORT rows);
void* console_override(const char* name);

// net.cpp
void* net_override(const char* name);
const char* wsock32_name(uint16_t ordinal);

// tls.cpp
bool tls_init(Image& img, std::string& err);
// For threads that belong to the program: installs its TLS until the thread ends.
void tls_ensure_thread();
// Runs the program's thread-detach callbacks and gives slot 0 back to Windows.
// Must be called before a program thread exits.
void tls_thread_exit();
// For threads the program borrows: installs its TLS until tls_leave.
void* tls_enter();
void tls_leave(void* previous);
void tls_run_callbacks(DWORD reason);

}  // namespace xb
