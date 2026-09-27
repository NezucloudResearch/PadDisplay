# PadDisplay by Nezucloud

Use an Android tablet as a **real extended monitor** for your Windows PC, over USB or Wi-Fi. It also supports multi-touch, touch-as-mouse, and a stylus with pressure, tilt and hover.

> **PadDisplay by Nezucloud** is an independent open-source project. It is **not related** to the iPadOS app "PadDisplay" by treastrain / Ryoga Tanaka, nor affiliated with NVIDIA, Microsoft, Google or Huawei (see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)).

## Features
- **A real second monitor,** not a mirror. Windows extends the desktop onto a virtual display, and the tablet shows it.
- **Low latency.**
  - Capture copies only the parts of the screen that changed, straight into the hardware encoder's input, all on the GPU.
  - Encoding uses NVENC (H.264/HEVC, ultra-low-latency) on NVIDIA GPUs, with a Media Foundation fallback for Intel and AMD.
  - The tablet uses its hardware decoder in low-latency mode.
  - Measured on an RTX 4050 Laptop at 2560×1600: about 3.3 ms per frame to encode (H.264), and up to 120 fps.
- **USB** (through `adb reverse`, set up automatically) **or Wi-Fi** (PCs are found automatically).
  - Flow control and adaptive bitrate keep lag bounded on slow networks.
- **Input.**
  - Real Windows multi-touch, or touch-as-mouse gestures.
  - The stylus acts as a Windows pen, with pressure, tilt, hover and the eraser, and palm rejection.
- **Optional PC audio on the tablet.** It's off by default; when off, nothing is captured or sent.
  - Raw 16-bit PCM (about 1.5 Mbps, no codec delay) on its own connection, so it never holds up the picture.
  - It can mute the PC's speakers while it plays.
- **Stays out of the way.**
  - The virtual monitor appears when PadDisplay runs and disappears when it exits.
  - Your monitor arrangement and each tablet's own setup are remembered.
  - Refresh rate (60/120 Hz) follows the tablet.

```
Windows host (C++20)                                               Android app (Kotlin)
Virtual Display Driver ─► DXGI Desktop Duplication (changed areas only) ─► cursor (GPU)
  ─► NVENC H.264/HEVC (zero-copy, ultra-low-latency CBR)   ──TCP──►  MediaCodec (low-latency) ─► SurfaceView
  ◄─ InjectSyntheticPointerInput (touch/pen) / SendInput   ◄──────  MotionEvents (touch, pen, mouse gestures)
USB = adb reverse tcp:27183 (automatic) · Wi-Fi = UDP discovery on 27184 · 6-digit PIN on both
```

## Requirements
- **PC:**
  - Windows 10 1809+ or Windows 11, x64.
  - An NVIDIA GPU is recommended (NVENC). Intel and AMD work through Media Foundation, but that path is less tested.
  - For USB mode, [Android platform-tools](https://developer.android.com/tools/releases/platform-tools) (`adb`), in `C:\android-platform-tools` or on PATH.
- **Tablet:** Android 10+ (API 29) with hardware H.264/HEVC decoding. Tested on a Huawei MatePad 11 (Snapdragon 865, 120 Hz).

## Install
1. **PC.** Unzip the Windows release (or your own `host\dist\PadDisplay\`) anywhere and run `PadDisplay.exe`. It goes to the tray.
   - The first time, click the notification, or **tray → Install virtual monitor driver (admin)**. That's one administrator prompt, and Windows may ask you to trust the driver publisher "SignPath Foundation".
   - This one-time step installs the bundled [Virtual Display Driver](https://github.com/VirtualDrivers/Virtual-Display-Driver) and writes `C:\VirtualDisplayDriver\vdd_settings.xml` (common tablet modes at 60/90/120/144 Hz).
   - It registers two hidden scheduled tasks (`\PadDisplay\VirtualMonitorOn` / `Off`, which run `pnputil` to switch the virtual monitor on or off without further prompts).
   - It sets the NVIDIA driver profile of `PadDisplay.exe` to "prefer maximum performance". That only takes effect while a tablet is connected.
2. **Tablet.** Install `PadDisplay.apk`: copy it over and open it, or run `adb install -r PadDisplay.apk`.
   - For USB, enable *Developer options → USB debugging*. On HarmonyOS, also enable *Allow ADB debugging in charge only mode*.

## Use
- **USB (lowest latency):** plug in the tablet, open PadDisplay and tap **Connect over USB**.
- **Wi-Fi:** tap your PC under *Wi-Fi*, or type its IP address.
- **PIN:** the first time, the app asks for the PIN shown in the PC's tray menu, then remembers it for that PC.
- **Input:**
  - The stylus is always a Windows pen. Fingers are ignored while the pen is near the screen.
  - With *Touch acts as mouse* on: tap = click, drag = drag, long-press or two-finger tap = right-click, two-finger drag = scroll.
- **Audio:** in the tablet app, turn on *Audio → Play PC audio on this tablet*.
  - *Mute the PC's speakers while playing here* (on by default) silences the PC and restores it when you disconnect.
  - Windows can only capture everything the PC plays, not just the apps on the tablet's screen.
- **Arrangement:** place the virtual monitor, and set its resolution, in *Windows Settings → Display* as usual. PadDisplay remembers it per tablet.
- **Exit** from the tray removes the virtual monitor, leaving only your real screens.

## Build from source
- **Host:** run `host\build.cmd`.
  - Needs Visual Studio 2022 Build Tools with the C++ workload, CMake and the Windows 11 SDK.
  - The output is `host\build\PadDisplay.exe`, plus a ready-to-copy package in `host\dist\PadDisplay\`.
- **Android:** run `cd android` then `gradlew assembleRelease`.
  - Needs JDK 17 and the Android SDK (platform 35, build-tools 35.0.0), with `sdk.dir` set in `android/local.properties`.
  - Release builds are signed with the debug key, for sideloading. Use your own keystore for store releases, and never commit it.
- **CI:** `.github/workflows/build.yml` builds both on every push.

Code layout:
- `host/src` is the Windows tray app.
- `android/app/src/main/java/com/nezucloud/paddisplay` is the tablet app.
- `docs/PROTOCOL.md` is the wire protocol.

## Tuning (`%LOCALAPPDATA%\PadDisplay\settings.ini`)
| key | default | notes |
|---|---|---|
| `codec` | `auto` | `auto` means H.264 over USB (fastest encode) and HEVC over Wi-Fi (better quality per bit) |
| `usbBitrateMbps` / `wifiBitrateMbps` | 40 / 25 | caps; also in the tray menu |
| `adaptive` | 1 | adaptive bitrate. Wi-Fi starts at 60% of the cap and follows the link |
| `maxFramesInFlight` | 3 | frames not yet acknowledged by the tablet. This bounds lag on slow links (see `docs/PROTOCOL.md`) |
| `maxFps` | 240 | ceiling. The tablet picks its refresh rate (app → *Display → Refresh rate*) |
| `matchClientResolution` | 1 | on a tablet's first connection, use its native resolution. After that PadDisplay restores the setup you last used for that tablet (`virtual-monitor.txt`) |
| `detachOnDisconnect` | 0 | remove the virtual monitor from the desktop when the tablet disconnects |
| `removeMonitorOnExit` | 1 | switch the virtual monitor off when PadDisplay exits |
| `requireUsbPin` | 1 | ask for the PIN over USB too (see [SECURITY.md](SECURITY.md)) |
| `gpuMaxPerformance` | 1 | NVIDIA "prefer maximum performance" profile, applied by the admin setup step. Without it, the GPU idles between light updates and encoding slows from about 3 ms to 13–15 ms |
| `nvencPreset` | `0` (auto) | auto = H.264 P2 / HEVC P3 |
| `encoder` | `auto` | `nvenc` / `mf` (Media Foundation) |
| `wifiQos` | 0 | tag the Wi-Fi stream as video (qWAVE). Off by default, because some routers drop the tagged packets |
| `display` | empty | force a specific `\\.\DISPLAYn` |
| `profile` | 0 | diagnostics: GPU copy time and the share of the screen copied per frame, in the stats line |

Diagnostics:
- `PadDisplay.exe --list` shows the displays, the adapters and the virtual display's modes.
- `PadDisplay.exe --dump out.h264 --seconds 10 [--fps 120] [--codec hevc]` captures and encodes without a tablet.
- The PC log is `%LOCALAPPDATA%\PadDisplay\host.log`, with a stats line every 10 s.
- On the tablet: `adb logcat -s PadStats PadConn`. Huawei ROMs hide app logs until you run `adb shell setprop log.tag.PadStats V`.

## Known limits
- Desktop Duplication can't capture the UAC secure desktop or DRM-protected video; those show as black or frozen.
- Input can't reach apps running as administrator unless PadDisplay also runs elevated (a Windows UIPI rule).
- The picture is landscape only.
- **The stream is not encrypted.** See [SECURITY.md](SECURITY.md).
- On HarmonyOS, exclude PadDisplay from battery optimisation for long Wi-Fi sessions.
- **Audio delay:**
  - Measured parts: about 10 ms of Windows capture period, 1–5 ms on the network, and a tablet jitter buffer that targets 30 ms. The target grows up to 80 ms by itself on a jittery Wi-Fi link.
  - The tablet's own output delay adds to that, and the total wasn't measured end to end.
  - The PC and tablet audio clocks drift slightly apart. That is corrected by playing up to 1–2% faster or slower, which you can't hear, instead of dropping audio.
  - Audio is only dropped if the tablet falls more than 200 ms behind, for example when its audio system stalls.
  - If PadDisplay crashes while "mute PC" is on, unmute the PC by hand.

## License
[MIT](LICENSE) © 2026 Nezucloud. Bundled third-party components keep their own licenses; see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). Contributions are welcome; see [CONTRIBUTING.md](CONTRIBUTING.md).
