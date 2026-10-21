// PadDisplay by Nezucloud. Copyright (c) 2026 Nezucloud.
// SPDX-License-Identifier: MIT
#pragma once
#include <cstdint>
#include <string>

namespace pd {

// Persisted in %LOCALAPPDATA%\PadDisplay\settings.ini.
struct Settings {
    int usbBitrateMbps = 40;     // cap when the client connects over USB (adb reverse / loopback)
    int wifiBitrateMbps = 25;    // cap for Wi-Fi clients
    int minBitrateMbps = 4;      // adaptive floor
    bool adaptive = true;        // lower bitrate when frames queue up
    int maxFramesInFlight = 3;
    bool wifiQos = false;        // qWAVE audio/video tagging; off: some routers drop the tagged frames   // frames sent but not yet acked by the tablet; bounds network queueing
    std::wstring codec = L"auto";// auto (H.264 on USB, HEVC on Wi-Fi) | hevc | h264
    int maxFps = 240;            // ceiling; the tablet picks its refresh rate (e.g. 60 or 120)
    std::wstring encoder = L"auto"; // auto | nvenc | mf
    std::wstring gpu = L"auto";     // auto | the name of one GPU (tray menu "Encode on"): which GPU renders the virtual monitor, and so encodes it
    int nvencPreset = 0;         // 0 = auto, 1 (fastest) .. 7 (best quality)
    bool gamingMode = false;     // realtime GPU priority: a game using the whole GPU cannot starve capture/encode.
                                 // Chosen in the tablet app; this value is for apps older than protocol v3 and --dump
    bool drawCursor = true;
    bool matchClientResolution = true;
    bool monitorOnlyWhileConnected = true; // the virtual monitor (the driver device) is switched off while no tablet is connected, so Windows
                                           // shows only the real screens; on when a tablet connects, off again when it leaves
    bool removeMonitorOnExit = true;
    bool gpuMaxPerformance = true;   // NVIDIA profile "prefer maximum performance" for PadDisplay.exe (set at install)
    bool profile = false;            // time GPU compose separately from encode (diagnostics; adds a GPU sync) // disable the virtual monitor when PadDisplay exits
    std::wstring displayOverride; // e.g. \\.\DISPLAY5; empty = auto-detect the virtual display
    std::wstring adbPath;         // empty = auto
    uint32_t pin = 0;             // pairing PIN, generated on first run
    bool runAsAdmin = false;      // start elevated: Windows only lets an elevated program send input to elevated windows (Task Manager...)
    bool autostartTask = false;   // "Start with Windows" is a scheduled task (starts elevated without a prompt), not a Run key
    bool requireUsbPin = true;   // USB (adb reverse) is reachable by any app on the tablet, so ask for the PIN there too
    bool acceptTabletMic = true;     // let the tablet's microphone become a PC microphone (the tablet decides whether to send it)
    std::wstring micBackend = L"auto"; // auto (PadDisplay's driver where Windows loads it, else VB-CABLE) | driver | cable
    std::wstring micDevice;          // VB-CABLE backend: part of the playback device's name; empty = "CABLE Input"
    bool cableMinPeriod = false;     // VB-CABLE backend: the audio engine's smallest period (IAudioClient3, about 35 ms less delay; measured to starve
                                     // other threads on a 2-core VM, so off by default); off = the default 10 ms period
    bool acceptTabletCamera = true;  // let the tablet's camera show up as the PC webcam (Windows 11)
    bool requireEncryption = false; // refuse plain (unencrypted) connections that do not come over USB; off so tablet apps from before TLS still connect
    bool uacClickableOnTablet = false; // only-screen + elevated: move UAC prompts off the (uncapturable) secure desktop so the
                                       // tablet can show and click them. Machine-wide, security-relevant (SECURITY.md); off by default
    bool lockScreenControl = false;    // elevated: let a paired tablet see and type into the Windows lock / sign-in screen (the
                                       // Winlogon secure desktop) through a System helper - it can enter the Windows password.
                                       // A bigger surface than clicking a UAC prompt, so its own opt-in. Security-relevant (SECURITY.md); off by default
    bool preSignInAccess = false;      // a LocalSystem service serves the tablet before anyone signs in (at boot / after sign-out),
                                       // Wi-Fi + TLS only, so the tablet can sign in from a cold boot. Installed by --install-service.
                                       // The largest surface (always-on System listener); off by default. Security-relevant (SECURITY.md)

    void Load();
    void Save() const;
};

std::wstring AppDataDir();

} // namespace pd
