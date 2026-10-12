# Contributing

Thanks for helping! Issues and pull requests are welcome.

- **Bugs:** include the PC log (`%LOCALAPPDATA%\PadDisplay\host.log`), your GPU, your Windows version, the tablet model and Android version, and whether you used USB or Wi-Fi.
- **Code style:** match the surrounding code.
  - The host is C++20 and must build without warnings (`/W4`).
  - The app is Kotlin, using the official Kotlin style.
- **Performance changes:** include before/after numbers. `PadDisplay.exe --dump NUL --seconds 10 --fps 120` together with `profile=1` in `settings.ini` gives you per-second copy and encode timings.
- **Installer changes:** `host\installer\build-installer.cmd` builds the installer and the portable zip into `host\dist\` (needs [Inno Setup 6](https://jrsoftware.org/isinfo.php): `winget install JRSoftware.InnoSetup`). The installer only copies files and then runs `PadDisplay.exe` with one setup command per ticked box (`kSetupCommands` in `host/src/main.cpp`), so a new component is a new command there, a line in `PadDisplay.iss`, and its removal in `RunUninstallSetup`. Test installs and uninstalls in a virtual machine, not on your own PC: they add and remove drivers.
- **Protocol changes** (`host/src/protocol.h` and `android/.../Protocol.kt`) must stay in sync, and must be documented in `docs/PROTOCOL.md`. Bump the protocol version if old clients would break.
- **License:** by contributing you agree that your contribution is licensed under the MIT License of this project.
