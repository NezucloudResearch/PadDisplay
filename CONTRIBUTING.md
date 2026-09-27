# Contributing

Thanks for helping! Issues and pull requests are welcome.

- **Bugs:** include the PC log (`%LOCALAPPDATA%\PadDisplay\host.log`), your GPU, your Windows version, the tablet model and Android version, and whether you used USB or Wi-Fi.
- **Code style:** match the surrounding code.
  - The host is C++20 and must build without warnings (`/W4`).
  - The app is Kotlin, using the official Kotlin style.
- **Performance changes:** include before/after numbers. `PadDisplay.exe --dump NUL --seconds 10 --fps 120` together with `profile=1` in `settings.ini` gives you per-second copy and encode timings.
- **Protocol changes** (`host/src/protocol.h` and `android/.../Protocol.kt`) must stay in sync, and must be documented in `docs/PROTOCOL.md`. Bump the protocol version if old clients would break.
- **License:** by contributing you agree that your contribution is licensed under the MIT License of this project.
