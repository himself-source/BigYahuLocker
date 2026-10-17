# ScreenLocker

A single-file Windows screen locker written in C++. Fullscreen overlay,
low-level keyboard hook, password gate, Run-key persistence, and a watchdog
that respawns the process if it dies.

Built as a compact Win32 reference for kiosk-style lockdown and self-lock
scenarios. No dependencies beyond the Windows SDK.

## Features

- Fullscreen `WS_EX_TOPMOST` overlay, re-asserted every 200 ms
- Blocks Alt+Tab, Alt+F4, Alt+Esc, Ctrl+Esc, Win keys, Apps key, Ctrl+Alt+Del (best-effort)
- Password unlock (default: `netanyahu`) via masked edit box
- Persistence: writes itself to `HKCU\...\Run` on start, removes on clean unlock
- Watchdog thread respawns the process if the main instance is killed
- Single-instance guard via named mutex
- Optional background image (`bg.bmp` next to the exe), falls back to solid black

## Build

**Fedora cross-compile (MinGW-w64):**

```bash
sudo dnf install mingw64-toolchain
x86_64-w64-mingw32-g++ -EHsc -O2 -DUNICODE -D_UNICODE locker.cpp -o locker.exe \
  -luser32 -lgdi32 -ladvapi32 -lmsimg32 -mwindows
