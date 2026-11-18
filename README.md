# Vagii Mouse

Pointer speed beyond Windows' 20 levels, for the mouse and the trackpad separately. Per-user, **no admin needed**.

## Install
Download `VagiiMouse-Setup.exe` from Releases and run it (installs to `%LOCALAPPDATA%\Programs\VagiiMouse`, no admin).
Or just run the portable `VagiiMouse.exe` directly.

## How it works
- **Mouse tab**: levels 1–20 set Windows' own pointer speed; 21–100 keep Windows at 20 and multiply every movement
  (level 100 = 6x Windows' max) through a low-level mouse hook. Acceleration: Off, Windows (Enhance pointer precision),
  or the adjustable Vagii boost curve. Shows the effective DPI for your mouse's DPI.
- **Trackpad tab**: levels 1–20 set Windows' precision-touchpad cursor speed (Windows 11 24H2+); 21–100 add the boost
  on top. Trackpad moves are recognised from the touchpad's own raw HID reports. A green dot on each tab shows which
  device is moving the pointer.
- **Reset to Windows default** and **Restore my original settings** per tab.
- **Ctrl+Alt+M** turns the boost on/off. Boost pauses automatically when a game hides or locks the cursor.
- Lives in the tray; the boost only works while the app is running. Optional start with Windows.

Settings: `%LOCALAPPDATA%\VagiiMouse\settings.ini`.

## Build
Needs Visual Studio Build Tools (C++). `build.ps1` -> `bin\VagiiMouse.exe` (static CRT).
`build-installer.ps1` also builds the Inno Setup installer (`installer\VagiiMouse.iss`).
