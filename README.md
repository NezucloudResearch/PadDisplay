# PadDisplay by Nezucloud

Use an Android tablet as a **real extended monitor** for your Windows PC, over USB or Wi-Fi. It also supports multi-touch, touch-as-mouse, a stylus with pressure, tilt and hover, and a keyboard, mouse and game controllers connected to the tablet.

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
  - A keyboard, mouse and game controllers connected to the tablet (Bluetooth or USB) work on the PC as if they were plugged into it. Controllers appear as Xbox 360 controllers, with rumble.
- **Optional PC audio on the tablet.** It's off by default; when off, nothing is captured or sent.
- **Optional tablet microphone and camera as PC devices.** Two switches in the tablet app, both off by default: the tablet's microphone shows up as a PC microphone (through PadDisplay's own driver where Windows allows it, otherwise through the free VB-CABLE that comes with PadDisplay), and its camera shows up as a webcam (Windows 11). Nothing is captured while they are off, and only the tablet can turn them on.
  - Raw 16-bit PCM (about 1.5 Mbps, no codec delay) on its own connection, so it never holds up the picture.
  - It can mute the PC's speakers while it plays.
- **Stays out of the way.**
  - The virtual monitor exists only while a tablet is connected. It is switched on when a tablet connects (this adds about 2 seconds to the first picture) and off again when the tablet leaves, and your screens go back to exactly how you had them. While PadDisplay runs with no tablet, Windows shows only your real screens.
  - Your monitor arrangement and each tablet's own setup are remembered.
  - Refresh rate (60/120 Hz) follows the tablet.

```
Windows host (C++20)                                               Android app (Kotlin)
Virtual Display Driver ─► DXGI Desktop Duplication (changed areas only) ─► cursor (GPU)
  ─► NVENC H.264/HEVC (zero-copy, ultra-low-latency CBR)   ──TCP──►  MediaCodec (low-latency) ─► SurfaceView
  ◄─ InjectSyntheticPointerInput (touch/pen) / SendInput   ◄──────  MotionEvents (touch, pen, mouse gestures)
  ◄─ SendInput (keys, mouse) / ViGEmBus (Xbox 360 pads)    ◄──────  keyboard, mouse, game controllers
USB = adb reverse tcp:27183 (automatic) · Wi-Fi = UDP discovery on 27184, encrypted with TLS · 6-digit PIN on both
```

## Requirements
- **PC:**
  - Windows 10 1809+ or Windows 11, x64.
  - An NVIDIA GPU is recommended (NVENC). Intel and AMD work through Media Foundation, but that path is less tested.
  - For USB mode, [Android platform-tools](https://developer.android.com/tools/releases/platform-tools) (`adb`), in `C:\android-platform-tools` or on PATH.
  - For game controllers, the [ViGEmBus](https://github.com/nefarius/ViGEmBus) driver. It ships with PadDisplay and is installed by the admin setup step; the keyboard and mouse work without it.
  - For the tablet's microphone: Windows 10 or later. PadDisplay's own microphone driver (Windows 10 2004 or later) and VB-CABLE both ship with PadDisplay and are installed on request (tray menu, one administrator prompt). For the tablet's camera: Windows 11.
- **Tablet:** Android 10+ (API 29) with hardware H.264/HEVC decoding. Tested on a Huawei MatePad 11 (Snapdragon 865, 120 Hz).

## Install
1. **PC.** Run `PadDisplay-Setup-<version>.exe`. It asks for administrator rights once, installs PadDisplay into Program Files and sets up what you leave ticked:
   - **Virtual monitor:** the bundled [Virtual Display Driver](https://github.com/VirtualDrivers/Virtual-Display-Driver), which makes the tablet a second screen. This also writes `C:\VirtualDisplayDriver\vdd_settings.xml` (common tablet modes at 60/90/120/144 Hz), registers two hidden scheduled tasks (`\PadDisplay\VirtualMonitorOn` / `Off`, which run `pnputil` to switch the virtual monitor on or off without further prompts) and sets the NVIDIA driver profile of `PadDisplay.exe` to "prefer maximum performance" (that only takes effect while a tablet is connected).
   - **Game controllers:** the ViGEmBus driver.
   - **The tablet's camera as a webcam** (Windows 11).
   - **The tablet's microphone:** VB-CABLE, a virtual audio cable by VB-Audio (donationware, [vb-cable.com](https://vb-cable.com)). It adds a recording device, *CABLE Output*, and a playback device, *CABLE Input* (see *Microphone* below).
   - **Start PadDisplay with administrator rights when I sign in, without a prompt:** the tablet can then use Task Manager and other administrator windows (see *Known limits*). Untick it and PadDisplay runs with your normal rights; *Start with Windows* and *Run as administrator* stay in the tray menu. The start is registered for the Windows account that runs the installer.
   - **Trust SignPath Foundation:** the virtual monitor driver is signed by SignPath Foundation, and Windows asks before it installs a driver from a publisher it has not been told to trust. With this ticked the installer adds that publisher to Windows' Trusted Publishers, so nothing asks. The price: Windows then installs any driver signed by SignPath Foundation without asking (see [SECURITY.md](SECURITY.md)). Untick it and Windows asks once; answer **Install**. The other bundled drivers are signed by Microsoft, and Windows never asks about those.
   - Whatever you untick can be set up later from the tray menu (**Finish PadDisplay setup**, and the microphone and camera items).
   - The installer is not code-signed, so Windows SmartScreen may warn about an unknown publisher when you start it.
   - **Uninstall** in *Windows Settings → Apps*. It removes PadDisplay, its scheduled tasks, the virtual camera and the drivers the installer added, and takes SignPath Foundation out of the Trusted Publishers again if the installer put it there. A driver that was on the PC before PadDisplay (your own ViGEmBus or VB-CABLE, say) stays. It asks whether to delete your settings too. Two things outlast it until the next restart or longer: the virtual camera's file in `Program Files\PadDisplay\Camera` if Windows' camera service still has it loaded (Windows deletes it at the restart), and VB-CABLE's package in Windows' driver store, which VB-Audio's uninstaller leaves.
   - **Without an installer:** unzip `PadDisplay-<version>-portable.zip` (or your own `host\dist\PadDisplay\`) anywhere and run `PadDisplay.exe`. The first time, click the notification, or **tray → Install virtual monitor driver (admin)**: one administrator prompt, and Windows asks whether to trust the driver publisher "SignPath Foundation". A portable copy cannot start with administrator rights at sign-in without a prompt: that needs a folder only administrators can change, which is what the installer provides.
2. **Tablet.** Install `PadDisplay.apk` (it is in the PadDisplay folder: *Start menu → PadDisplay → Tablet app (APK) and documents*): copy it over and open it, or run `adb install -r PadDisplay.apk`.
   - For USB, enable *Developer options → USB debugging*. On HarmonyOS, also enable *Allow ADB debugging in charge only mode*.

## Use
- **USB (lowest latency):** plug in the tablet, open PadDisplay and tap **Connect over USB**.
- **Wi-Fi:** tap your PC under *Wi-Fi*, or type its IP address.
- **PIN:** the first time, the app asks for the PIN shown in the PC's tray menu, then remembers it for that PC.
- **Encryption:** over Wi-Fi the stream is encrypted with TLS. The first time, the app also remembers the PC's identity (its fingerprint is in the tray menu), and warns you if it ever changes. USB is not encrypted: it never touches the network.
- **Input:**
  - The stylus is always a Windows pen. Fingers are ignored while the pen is near the screen.
  - With *Touch acts as mouse* on: tap = click, drag = drag, long-press or two-finger tap = right-click, two-finger drag = scroll.
- **Keyboard, mouse and game controllers:** connect them to the tablet (Bluetooth or USB) and they work on the PC. Turn this off with *Send keyboard, mouse and game controllers to the PC*.
  - **Keyboard:** the PC's keyboard layout applies, not the tablet's.
  - **System shortcuts (Alt+Tab, the Windows key, Ctrl+Esc, Alt+F4 and so on):** Android normally takes these for itself. To send them to the PC instead, switch on the **PadDisplay keyboard shortcuts** service once in Android's accessibility settings (the tablet app has a button for it, under *Input*). While PadDisplay is open and connected, every key goes to the PC and none of them acts on the tablet; leave PadDisplay (or pull down the notification shade) and Android has its keys back. Turn it off with *Send the keyboard's system shortcuts too*. The service reads no screen content and is only asked for keys during a session. On Android 13 and later, a sideloaded app may first need *Allow restricted settings* in its App info page.
  - **Mouse:** the PC pointer follows the mouse over the picture. For games, turn on *Capture the mouse for games*, or press **Ctrl+Alt+Shift+M** while connected: the mouse then sends its raw movement, and the pointer can leave the tablet's screen.
  - **Game controllers:** up to four, each shown to Windows as an Xbox 360 controller. They need ViGEmBus on the PC:
    - If it's missing, the tablet and the PC's tray tell you as soon as a controller is used. **Tray → Install game controller driver (admin)** installs the copy that ships with PadDisplay (one administrator prompt); the same step is part of **Finish PadDisplay setup**.
    - If it's installed but switched off, PadDisplay switches it on by itself. That works once the admin setup step has run with ViGEmBus installed; until then, use **tray → Enable game controller driver (admin)**.
- **Audio:** in the tablet app, turn on *Audio → Play PC audio on this tablet*.
- **Microphone:** in the tablet app, turn on *Microphone and camera → Use this tablet's microphone as a PC microphone* (Android asks for the permission).
  - **It only runs while the PC uses it.** The tablet's microphone opens when an app on the PC starts recording from the virtual microphone (a call, a recorder, the level meter in Windows' sound settings) and closes 2 seconds after the last one stops. Android's microphone indicator and the tablet's "Microphone on" label show exactly that. The first fraction of a second after an app starts recording is silence, while the tablet's microphone starts.
  - **Which virtual microphone it uses.** PadDisplay's own driver (about 15 ms of delay) loads only where Windows accepts it: a PC with Secure Boot loads kernel drivers only when Microsoft has signed them, and PadDisplay's driver is not signed yet. So on a normal PC PadDisplay uses **VB-CABLE** instead, a free virtual audio cable by VB-Audio (donationware, [vb-cable.com](https://vb-cable.com)) that comes with PadDisplay. The installer sets it up if you leave *The tablet's microphone* ticked. Otherwise nothing is installed without asking: when you tick **Accept the tablet's microphone** in the tray menu, or the tablet asks for its microphone, PadDisplay offers to install VB-CABLE and you say yes (its driver is signed by Microsoft, so Windows does not ask again). Then, in Zoom, Teams, a browser or any recording app, choose **CABLE Output** as the microphone. VB-CABLE also adds a playback device called *CABLE Input* that you can ignore. A cable adds delay: in a test VM about 90-110 ms in all (93-106 ms of it is the cable and Windows' audio engine alone), against about 15 ms with PadDisplay's own driver. VB-CABLE is donationware: please support it at [vb-cable.com](https://vb-cable.com). Uninstalling PadDisplay removes it if PadDisplay installed it; to remove it on its own, use VB-Audio's uninstaller (VBCABLE_Setup_x64.exe -u, in driver\vbcable). VB-Audio recommends a restart after installing or removing it: installing it again right after removing it, without a restart in between, can leave its device not working until the restart (seen in tests).
  - **PadDisplay's own driver** is used automatically on a PC where Windows loads it (test-signing on, or once a build signed by Microsoft exists). Install it with **tray → Install microphone driver (admin)**, then choose **Microphone (PadDisplay Virtual Microphone)** as the microphone. It carries the tablet's microphone while a tablet is connected with the switch on, and silence otherwise. The driver is only loaded while PadDisplay is running with **Accept the tablet's microphone** ticked in the tray menu: unticking it, or exiting PadDisplay, switches the driver off (and the microphone disappears from your apps) until you tick it again. If an app is recording from the microphone at that moment, the driver stays loaded until that app stops (Windows would otherwise leave the device stuck until a restart); after Exit it is switched off the next time PadDisplay starts. **Tray → Remove microphone driver (admin)** takes it out again. It is a kernel driver, and PadDisplay will not turn off Secure Boot or enable test-signing for it (see [acxmic's signing notes](https://github.com/NezucloudResearch/acxmic/blob/main/docs/SIGNING.md); the driver is its own project, [acxmic](https://github.com/NezucloudResearch/acxmic)).
  - With echo cancellation on the tablet, PC audio playing from the tablet's speaker isn't sent back to the PC.
- **Camera (Windows 11):** in the tablet app, turn on *Microphone and camera → Use this tablet's camera as a PC webcam*, and choose front or back and 720p or 1080p.
  - The first time, run **tray → Set up the tablet's camera as a webcam (admin)** (also part of **Finish PadDisplay setup**): one administrator prompt registers the virtual camera, which then appears as **PadDisplay Camera (Windows Virtual Camera)** in every camera app.
  - The tablet's camera only turns on while an app on the PC has the webcam open, and a "Camera on" label shows on the tablet. When no tablet is connected, apps see a dark picture with a moving bar.
  - *Mute the PC's speakers while playing here* (on by default) silences the PC and restores it when you disconnect.
  - Windows can only capture everything the PC plays, not just the apps on the tablet's screen.
- **Arrangement:** place the virtual monitor, and set its resolution, in *Windows Settings → Display* as usual. PadDisplay remembers it per tablet.
- **Extra screen or only screen:** in the tablet app, *Display → Use this tablet as the PC's only screen*.
  - Off: the tablet is an extra monitor next to the PC's own.
  - On: the PC's own screens are switched off while the tablet is connected, and everything moves to the tablet. They come back, arranged as they were, when it disconnects, when PadDisplay exits, and after a restart. A monitor that was dark for a while needs a second or two to wake up.
- **Switching apps on the tablet:** PadDisplay stays connected while another app is in front or the tablet's screen is off (*Display → Stay connected in the background*, on by default). PC audio, the microphone and the camera keep running, the PC keeps the virtual monitor, and the picture is back as soon as you return. A notification shows meanwhile: tap it to go back, or tap *Disconnect*. While nothing is shown the tablet still receives the picture and throws it away, so disconnect when you are done to save battery. Turn the switch off to disconnect whenever you leave PadDisplay, as before.
- **Gaming mode:** in the tablet app, *Display → Gaming mode*. Turn it on if the picture stutters while a game uses the whole GPU. With little free video memory it can freeze the NVIDIA encoder, so it's off by default.
- **Exit** from the tray removes the virtual monitor, leaving only your real screens.

## Build from source
- **Host:** run `host\build.cmd`.
  - Needs Visual Studio 2022 Build Tools with the C++ workload, CMake and the Windows 11 SDK.
  - Clone with `git clone --recurse-submodules` (or run `git submodule update --init` afterwards): the microphone driver is its own repository, [acxmic](https://github.com/NezucloudResearch/acxmic), included as `host/third_party/padmic`. The host build only needs its header, not the Windows Driver Kit. Building the driver itself is described in the acxmic repository.
  - The output is `host\build\PadDisplay.exe`, plus a ready-to-copy package in `host\dist\PadDisplay\`.
  - `host\installer\build-installer.cmd` does the same and then builds `host\dist\PadDisplay-Setup-<version>.exe` and the portable zip. It needs [Inno Setup 6](https://jrsoftware.org/isinfo.php) (`winget install JRSoftware.InnoSetup`).
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
| `monitorOnlyWhileConnected` | 1 | keep the virtual monitor switched off while no tablet is connected (needs the admin setup step, which registers the on/off tasks). Set to 0 to keep it on whenever PadDisplay runs |
| `removeMonitorOnExit` | 1 | switch the virtual monitor off when PadDisplay exits |
| `requireUsbPin` | 1 | ask for the PIN over USB too (see [SECURITY.md](SECURITY.md)) |
| `acceptTabletMic` | 1 | let the tablet's microphone become a PC microphone (the tablet still has to switch it on) |
| `micBackend` | auto | `auto` (PadDisplay's driver where Windows loads it, otherwise VB-CABLE), `driver` or `cable` to force one |
| `micDevice` | (empty) | VB-CABLE backend: part of the playback device's name; empty = `CABLE Input` |
| `cableMinPeriod` | 0 | VB-CABLE backend: use the audio engine's smallest period (about 35 ms less delay; measured to starve other threads on a 2-core VM) |
| `acceptTabletCamera` | 1 | let the tablet's camera be the PC webcam (Windows 11, after the one-time setup) |
| `requireEncryption` | 0 | refuse Wi-Fi connections that are not encrypted. Off so tablet apps from before TLS (v0.3.0 and older) can still connect; turn it on once all your tablets are updated |
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
  - **UAC prompts** (for example when you start an app as administrator) are shown on that secure desktop, so the tablet can't show them or answer them. The tablet gets a notice, stays connected, and the picture continues once the prompt is answered on the PC.
  - With the tablet as the PC's only screen, the PC's own screens are off, so answer the prompt with the PC's keyboard. If the tablet disconnects meanwhile, PadDisplay keeps trying to switch the PC's screens back on, and does as soon as the prompt is answered.
  - To **click UAC prompts from the tablet** in only-screen mode, run as administrator and turn on **tray → Let the tablet answer UAC prompts in only-screen mode**. While a tablet is the only screen, prompts then move to the normal (capturable) desktop so the tablet shows and clicks them. It weakens UAC for the whole PC while active and is put back on disconnect/exit — see [SECURITY.md](SECURITY.md).
- **Apps running as administrator** (Task Manager, anything started with *Run as administrator*) ignore input from a normal program, a Windows rule (UIPI) — only game controllers still reach them, because they go in through a driver (ViGEmBus). When the tablet's mouse, keyboard, touch or pen is blocked this way, the tablet shows a message saying so. The installer's *Start PadDisplay with administrator rights when I sign in* takes care of it; otherwise choose **tray → Run as administrator** to let the tablet's touch, pen, keyboard and mouse reach them too. That's one UAC prompt. If *Start with Windows* is on and PadDisplay was installed with its installer, it then starts elevated at sign-in without a prompt; a portable copy asks at every sign-in, because a program that normal software can replace must not get administrator rights by itself. Choose the item again to go back to normal.
- Without the *PadDisplay keyboard shortcuts* service (see *Keyboard* above), Android keeps some shortcuts for itself and they never reach the PC: Alt+Tab, the Windows/Meta key shortcuts and, on some tablets, Print Screen. With it, they go to the PC. Ctrl+Alt+Del can't be sent either way (Windows doesn't allow injecting it).
- A controller's sticks and triggers follow Android's standard layout. A controller that Android itself maps wrongly is wrong on the PC too.
- The picture is landscape only.
- If PadDisplay is killed while the tablet is the PC's only screen, the PC's own screens stay dark until you start PadDisplay again or restart Windows.
- A PadDisplay started with administrator rights can't be closed or scripted by a normal program (`WM_CLOSE`, `taskkill`): use its tray menu.
- **Encryption has limits:** over Wi-Fi the stream is TLS-encrypted, but USB and tablet apps from before TLS are plain, and the very first connection from a tablet trusts the PC it finds. See [SECURITY.md](SECURITY.md).
- On HarmonyOS, exclude PadDisplay from battery optimisation for long Wi-Fi sessions, and to stay connected in the background (some ROMs close background apps even with a notification showing).
- **Audio delay:**
  - Measured parts: about 10 ms of Windows capture period, 1–5 ms on the network, and a tablet jitter buffer that targets 30 ms. The target grows up to 80 ms by itself on a jittery Wi-Fi link.
  - The tablet's own output delay adds to that, and the total wasn't measured end to end.
  - The PC and tablet audio clocks drift slightly apart. That is corrected by playing up to 1–2% faster or slower, which you can't hear, instead of dropping audio.
  - Audio is only dropped if the tablet falls more than 200 ms behind, for example when its audio system stalls.
  - If PadDisplay crashes while "mute PC" is on, unmute the PC by hand.

## License
[MIT](LICENSE) © 2026 Nezucloud. Bundled third-party components keep their own licenses; see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). Contributions are welcome; see [CONTRIBUTING.md](CONTRIBUTING.md).
