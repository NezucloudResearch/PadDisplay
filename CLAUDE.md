# PadDisplay: instructions for Claude

An Android tablet as a real extended monitor for a Windows PC, plus its keyboard, mouse, controller, touch and pen input. **Latency is the product.** Treat every change as a possible regression of it.

- `host/` is the Windows tray app (C++20, `/W4`, no warnings allowed).
- `android/` is the tablet app (Kotlin, minSdk 29).
- `docs/PROTOCOL.md` is the wire protocol. `README.md`, `SECURITY.md`, `CONTRIBUTING.md` and `THIRD_PARTY_NOTICES.md` are user-facing and must stay true.

## Performance rules (the most important section)

1. **No bottlenecks.** Before writing code, say which thread runs it. Hot threads must never wait on anything slow:
   - Host: the capture/encode/send thread (`Session::StreamLoop`), the receive thread (`RecvLoop`, it sends the acks), the input thread (`InputLoop`), the encoder.
   - Tablet: the reader thread (`Connection`), the decoder output thread (`VideoDecoder`), the UI thread (touch and key events).
   - Never on a hot thread: starting a process, SetupAPI or registry or file I/O, driver calls that wait for hardware, `Sleep` polling, per-frame allocation or logging, a lock that something slow can hold.
   - Slow work goes to a worker thread. Use a "newest state wins" mailbox so stale input is dropped instead of queued; `GamepadBridge` is the model.
2. **Use the newest method that lowers latency, when the minimum targets allow it** (Windows 10 1809+, Android 10 / API 29). Keep a fallback for older systems, and say in the code comment why the newer path is faster. Check first whether a newer API exists; do not assume the one already in the code is the best.
   - Prefer events, completion ports and condition variables over polling. If a poll is unavoidable, justify the interval in a comment.
   - Avoid extra copies, per-message heap allocations and locks on the hot path.
   - Prefer gather I/O (`WSASend` with several buffers) over concatenating into a temporary buffer.
3. **Do not undo what is already tuned for latency.** Read the code before changing any of it:
   - Capture changed rectangles only (DXGI Desktop Duplication, `DuplicateOutput1`), copied into the encoder's own texture on the GPU.
   - NVENC ultra-low-latency, no B-frames, infinite GOP, CBR with a one-frame VBV, synchronous encode with a one-texture ring; Media Foundation as the fallback for Intel and AMD.
   - Ack-based flow control (`maxFramesInFlight`) and adaptive bitrate, so a slow link drops frames instead of building a queue.
   - `TCP_NODELAY`, DSCP marking, the 20 ms keepalive, a Wi-Fi high-performance lock on the tablet.
   - MediaCodec low-latency keys and Qualcomm vendor keys, and skipping straight to the newest decoded frame.
   - MMCSS thread classes, the high-resolution waitable timer (`SleepUs`), GPU scheduling priority, power-throttling opt-out.
4. **Measure; never claim a speed-up you did not measure.**
   - Compare against a baseline build: `git archive HEAD host | tar -x -C <scratchpad>` and build it separately.
   - Run both builds through the same load: a loopback client on `127.0.0.1:27183` that acknowledges every frame, with a load generator drawing on the virtual monitor.
   - Alternate the builds over at least 3 rounds and compare medians. Report fps, arrival-gap p99, host CPU % and the host's own `enc` / `frame` / `waited` numbers. Say how noisy the runs were.
   - Useful tools: `profile=1` in `settings.ini`, `PadDisplay.exe --dump NUL --seconds 10 --fps 120`, and `adb logcat -s PadStats` on the tablet (on Huawei first run `adb shell setprop log.tag.PadStats V`).
   - If something could not be measured or tested (real keyboard, real controller, driver missing), say so plainly.
5. **Idle matters too.** A change that adds wakeups, threads or polling has a CPU and battery cost on a laptop. Measure idle CPU.

## Build and run

- Host: `host\build.cmd` (VS 2022 Build Tools, CMake, Ninja). It also assembles `host\dist\PadDisplay\`. It cannot link while `PadDisplay.exe` is running: exit it from the tray first (or post `WM_CLOSE` to the window of class `PadDisplayTray`).
- Tablet: `cd android` then `gradlew assembleRelease`. Install with `adb install -r`; a Huawei tablet may ask for a tap on the tablet.
- A host build and a baseline build can both exist; run only one at a time (they share port 27183 and the settings in `%LOCALAPPDATA%\PadDisplay`).

## Correctness rules learned the hard way

- **Never let child processes inherit handles.** `adb devices` leaves a daemon running, and it once inherited the listening socket, so Exit hung and port 27183 stayed bound. `RunCapture` passes only its pipe (a handle list), and sockets are marked non-inheritable (`NoInherit`). Keep it that way for any new `CreateProcess`.
- **Exit must always exit and clean up:** the port, the virtual monitor, the virtual controllers, and the PC's own screens (only-screen mode). After touching any shutdown or session code, test tray Exit with a tablet connected, and with a controller plugged in.
- **Only-screen mode** must never be saved into Windows' display database, so a crash or a restart restores the real screens by itself. A UAC prompt (secure desktop) makes `SetDisplayConfig` fail with access denied (5) until it is answered, so one failed restore must never be final: `ShowOnAllAgain` retries in the background.
- **The secure desktop is no failure.** While `DuplicateOutput` says access denied the session keeps sending pings (`Heartbeat`) and waits, instead of letting the tablet time out or ending the session after 30 s.
- **Elevation:** Windows (UIPI) drops input to elevated windows from a normal process, so the tablet can only touch Task Manager if PadDisplay is elevated (tray *Run as administrator*, setting `runAsAdmin`). An elevated host also cannot be closed with `WM_CLOSE` or `taskkill` from a normal script, and its mutex cannot be opened by a normal process (`AcquireSingleInstance` treats access denied as "running"). Tests that need it go through an elevated helper and a UAC prompt the user clicks: ask first.
- **Protocol changes:** `host/src/protocol.h`, `android/.../Protocol.kt` and `docs/PROTOCOL.md` change together. A host must keep accepting older clients; bump `kProtocolVersion` when an old client would break.
- **New input capabilities are security-relevant:** update `SECURITY.md`.
- **Windows headers define macros** such as `small`, `near`, `far`, `min`, `max`. Do not use them as identifiers.
- **Line endings:** the repository is LF (`.gitattributes`), working copies on this machine are CRLF. When editing with scripts, keep the file's existing line endings.
- Comments say why, not what, and match the density of the surrounding code.

## Working rules for this machine

- The user is often at the PC while you work. Do not run tests that blank their screens, move their mouse or grab their keyboard unless they agreed. For display tests, start a detached watchdog first that restores the screens (`SetDisplayConfig` with `SDC_USE_DATABASE_CURRENT`), and start its timer *after* the slow steps.
- Tests that connect a client change `%LOCALAPPDATA%\PadDisplay\virtual-monitor.txt` (the host remembers where the virtual monitor was). Back it up before and restore it after.
- Test without a tablet: a loopback client with the PIN from `settings.ini`. Test with the tablet: `adb` (USB debugging on, and on HarmonyOS also "Allow ADB debugging in charge only mode"), `uiautomator dump` to find controls, `input tap` to press them.
- Before installing the app or restarting the host, check what is running; do not kill the user's session without asking.
- Do not commit or stage unless asked. The user stages their own changes.
- Put temporary scripts and benchmark output in the scratchpad directory, not in the repository.
