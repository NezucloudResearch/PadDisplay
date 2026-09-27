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
    int nvencPreset = 0;         // 0 = auto, 1 (fastest) .. 7 (best quality)
    bool drawCursor = true;
    bool matchClientResolution = true;
    bool detachOnDisconnect = false;
    bool removeMonitorOnExit = true;
    bool gpuMaxPerformance = true;   // NVIDIA profile "prefer maximum performance" for PadDisplay.exe (set at install)
    bool profile = false;            // time GPU compose separately from encode (diagnostics; adds a GPU sync) // disable the virtual monitor when PadDisplay exits
    std::wstring displayOverride; // e.g. \\.\DISPLAY5; empty = auto-detect the virtual display
    std::wstring adbPath;         // empty = auto
    uint32_t pin = 0;             // pairing PIN, generated on first run
    bool requireUsbPin = true;    // USB (adb reverse) is reachable by any app on the tablet, so ask for the PIN there too

    void Load();
    void Save() const;
};

std::wstring AppDataDir();

} // namespace pd
