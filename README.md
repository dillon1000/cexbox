# Cexbox

Cexbox runs the official Windows build of Claude Code natively on an Xbox Series X|S in Developer Mode. It's an Xbox app with a small Windows program loader inside: it maps `claude.exe` into its own process and stands in for the Windows functions Xbox apps aren't allowed to use. There's no emulator and no virtual machine.

Because the program running is Anthropic's own Claude Code, you sign in with your Claude account through Anthropic's normal sign-in flow, and your subscription works as it does on a PC. Cexbox never sees or stores your credentials. `claude.exe` isn't included in the package; the app downloads it from Anthropic and checks it against Anthropic's published checksum.

## Status

Milestone 2 is built but not yet tested on an Xbox. Milestone 1's loader is also still untested on hardware, so the first run will show whether the hard parts work: mapping the image, thread-local storage, executable memory for the JavaScript engine, and file access.

| Milestone | What works |
| --- | --- |
| 1 | Loading `claude.exe` and running one-shot commands such as `--version` |
| 2 | Interactive Claude Code in a terminal, keyboard and controller input, sign-in by QR code |
| 3 | The Bash tool, by running a native Windows shell (busybox-w32) through the same loader |
| 4 | `git.exe` and `node.exe`/npm through the loader, a folder on a USB drive as your projects |
| 5 | Remote access: SSH or Tailscale into the Xbox |

## How it works

```
Cexbox.exe (C# Xbox app)
 ├─ downloads claude.exe from downloads.claude.ai into LocalState\claude
 ├─ VtTerminal: an xterm-compatible screen, drawn with Win2D by TerminalView
 ├─ keyboard and controller input, sign-in link as a QR code
 └─ xbloader.dll (C++)
     ├─ maps claude.exe: sections, relocations, imports, delay imports
     ├─ registers its unwind table so exceptions work
     ├─ gives every thread a copy of claude.exe's static TLS
     ├─ a virtual console, so claude.exe sees a real terminal
     ├─ replaces functions that must act differently in an app (memory, modules, exit, threads)
     └─ generates stubs for functions the Xbox doesn't have at all
```

### The virtual console

Claude Code's interactive interface only starts when stdin and stdout are a console. Xbox apps don't have one, so `native/src/console.cpp` makes three event objects, gives them to `claude.exe` as its standard handles, and answers every console call made on them: modes, screen size, `ReadConsoleInputW`, `WriteConsoleW`, `WriteFile` and so on. The handles are real event objects because libuv waits on stdin to learn when input is ready. Copies made with `DuplicateHandle` are tracked too, since libuv duplicates stdin.

Output goes to the app's terminal emulator, `app/VtTerminal.cs`. Keystrokes come back as console key events carrying the characters a terminal would send, escape sequences included. `tools/vtcheck` replays recorded terminal output through the emulator and prints the resulting screen. Claude Code's trust prompt and main screen render correctly in it.

Every import is resolved in this order:

1. **A replacement** from `native/src/shims.cpp`, when the function must behave differently for a program loaded by hand. For example, `VirtualAlloc` with execute permission becomes `VirtualAllocFromApp` plus `VirtualProtectFromApp` (the only way an Xbox app may create executable memory), `GetModuleHandle(NULL)` returns `claude.exe` instead of the app, and `ExitProcess` ends the program without closing the app.
2. **The real function**, if the Xbox's copy of the DLL exports it.
3. **A generated stub** from `native/src/stubs.cpp`, which logs the call and returns the "not supported" result that suits its DLL (`STATUS_NOT_IMPLEMENTED` for `ntdll`, `E_NOTIMPL` for COM, and so on).

The loader itself only imports functions on Microsoft's list of APIs allowed in Xbox and Store apps (`WindowsApp.lib`), so it links only if it stays inside that list.

## What claude.exe needs

From `claude.exe` 2.1.325 (Windows x64, 372 MB, built with Bun). `tools/delayimports.py` and `tools/classify.py` reproduce this.

- 621 imports: 458 are on the Xbox allow-list and 163 aren't.
- Most of the 163 belong to Claude Code's own Windows sandbox feature (firewall rules, local user accounts, AppContainer profiles, services) and can safely report "not supported".
- `wsock32` (28) is the old Winsock 1 DLL; its functions are forwarded to `ws2_32` by ordinal.
- The ones that matter: 25 `ntdll` file functions (`NtCreateFile`, `NtQueryDirectoryFile` and others) that Bun uses for the file system. The loader uses the real ones if the Xbox exports them. If it doesn't, they need real implementations on top of allowed calls, and the loader log will show which ones were stubbed.
- Pseudo-console functions (3) are only needed for the Bash tool's terminal, milestone 3.

## Building

Pushes to `main` build on GitHub Actions (`.github/workflows/build.yml`), which runs on Windows:

1. Builds `native/` with CMake as a UWP DLL.
2. Lists the DLL's imports into `loader-imports.txt`, to check nothing outside the allow-list slipped in.
3. Generates placeholder logos and a test signing certificate.
4. Builds and signs the app package.

Download the `cexbox-package` artifact from the workflow run.

## Installing on the Xbox

1. Open the Device Portal at `https://<xbox-ip>:11443`.
2. Under My games & apps, choose Add and pick `Cexbox_0.1.0.0_x64.msix` from the artifact.
3. Add the `.appx` files from the artifact's `Dependencies\x64` folder as dependencies, then install.
4. In Dev Home, select Cexbox, press View details and set App type to **Game** for more memory.
5. Open Cexbox, press Download Claude Code, then Start Claude Code.

Each launch can run Claude Code once. Press Restart app to run it again.

## Using it

A USB keyboard works like it does in any terminal. With a controller:

| Button | Does |
| --- | --- |
| D-pad, left stick | Arrow keys |
| A | Enter |
| B | Esc |
| Y | Opens the text box at the bottom for the on-screen keyboard |
| View | Shows the sign-in link as a QR code |
| Menu | Ctrl+C |
| LB / RB | Scroll back and forward through history |

Shift+PageUp and Shift+PageDown also scroll.

### Signing in

1. Start Claude Code and choose to sign in with your Claude account (or run `/login`).
2. Claude Code can't open a browser on the Xbox, so it shows a link. Press the View button or the Sign-in link button to see it as a QR code.
3. Scan it with your phone and sign in there.
4. The page then shows a code. Type it into the text box at the bottom of the app and press Send.

## Logs

The app writes two files to `LocalState\logs`, which you can download from the Device Portal's File explorer (LocalAppData → Cexbox_… → LocalState → logs):

- `loader.log`: how each import was resolved, every stub that got called, and any crash with its address inside `claude.exe`.
- `output.log`: everything the program printed.

If the app closes during a run, open it again: it shows the end of both files from the last run.

## Known risks

- **Executable memory.** The app asks for the `codeGeneration` capability. The emulator app this design learned from uses a package name that poses as Microsoft Edge, which may be how it got permission to run generated code. If the Xbox refuses here, the loader log will say so when it tries to make `claude.exe`'s code executable.
- **Thread-local storage.** `claude.exe`'s TLS index is placed past the ones Windows has used so far. If Windows later loads another DLL with static TLS, threads that already exist lose `claude.exe`'s TLS and will crash. See `native/src/tls.cpp`.
- **Missing `ntdll` file functions.** See above.
- **Stub results.** A stub that returns the wrong kind of failure can send Bun down an unexpected path. The loader log lists every stub call.
