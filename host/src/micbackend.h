// PadDisplay by Nezucloud. Copyright (c) 2026 Nezucloud.
// SPDX-License-Identifier: MIT
// Which virtual microphone carries the tablet's microphone on this PC. PadDisplay's own driver (acxmic) is a kernel driver:
// Windows loads it only if Microsoft signed it, or if test-signing is on (never turned on by PadDisplay). Where it cannot
// load, VB-CABLE (shipped with PadDisplay, installed on request) is used instead.
#pragma once
#include "settings.h"

#include <string>

namespace pd {

enum class MicBackend { Driver, Cable, None };

struct MicChoice {
    MicBackend backend = MicBackend::None;
    bool driverInstalled = false; // PadDisplay's driver device exists and is not in a problem state
    bool driverLoadable = false;  // a driver package is shipped and Windows will load it here (Microsoft-signed, or test-signing is on)
    bool cableInstalled = false;  // VB-CABLE's playback device is present
    std::wstring detail;          // one line for the log
};

// settings.micBackend: "auto" (the rule above), "driver" or "cable" (force one, for tests and support).
// Reads the device state and the shipped package: not for hot threads.
MicChoice ChooseMicBackend(const Settings& settings);

// Called (from any thread) when the tablet's microphone is wanted and there is no backend to carry it: the tray offers to install
// VB-CABLE. Defined by the program (main.cpp).
void OnMicSetupNeeded();

} // namespace pd
