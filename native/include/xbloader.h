// Public API of xbloader.dll, called from the C# app with P/Invoke.
#pragma once
#include <stdint.h>
#include <wchar.h>

#ifdef XBLOADER_EXPORTS
#define XB_API __declspec(dllexport)
#else
#define XB_API __declspec(dllimport)
#endif

#ifdef __cplusplus
extern "C" {
#endif

// Values returned by xb_poll.
enum {
    XB_NOT_STARTED = 0,
    XB_STARTING = 1,
    XB_RUNNING = 2,
    XB_EXITED = 3,
    XB_FAILED = 4,
};

// Sets an environment variable for the program before it starts. Returns 0 or a Win32 error.
XB_API int __stdcall xb_setenv(const wchar_t* name, const wchar_t* value);

// Flags for xb_start.
enum {
    // Give the program a virtual console instead of pipes, so it sees a terminal.
    XB_FLAG_CONSOLE = 1,
};

// Loads exe_path into this process and starts it on its own thread.
// command_line is the full command line, program name included.
// log_dir receives loader.log and output.log. Only one program can run per app launch.
// Returns 0 on success; on failure, xb_last_error describes what went wrong.
XB_API int __stdcall xb_start(const wchar_t* exe_path, const wchar_t* command_line,
                              const wchar_t* work_dir, const wchar_t* log_dir, int flags);

// Sends keystrokes (UTF-8, terminal escape sequences included) to the virtual console.
XB_API int __stdcall xb_console_input(const uint8_t* buffer, int length);

// Sets the virtual console's size in character cells. Can be called before xb_start.
XB_API int __stdcall xb_console_resize(int columns, int rows);

// Copies up to capacity bytes of the program's stdout and stderr (UTF-8). Returns the count.
XB_API int __stdcall xb_read_output(uint8_t* buffer, int capacity);

// Writes bytes to the program's stdin. Returns the count written, or -1.
XB_API int __stdcall xb_write_input(const uint8_t* buffer, int length);

// Returns one of the XB_ states above and the exit code once the program has exited.
XB_API int __stdcall xb_poll(int* exit_code);

// Copies the last error message. Returns its length.
XB_API int __stdcall xb_last_error(wchar_t* buffer, int capacity);

#ifdef __cplusplus
}
#endif
