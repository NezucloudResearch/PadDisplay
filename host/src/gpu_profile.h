// PadDisplay by Nezucloud. Copyright (c) 2026 Nezucloud.
// SPDX-License-Identifier: MIT
// NVIDIA driver profile for PadDisplay.exe: "Power management mode = Prefer maximum performance".
// Without it the laptop GPU drops to its lowest power state (P8, 405 MHz memory) between sparse
// updates, and NVENC then needs 13-15 ms per 2560x1600 frame instead of ~3.5 ms (measured). The
// driver applies the profile only while PadDisplay holds a D3D device, i.e. during a tablet session.
#pragma once
#include <string>

namespace pd {

enum class GpuProfileState { NoNvidia, NotSet, Set };

GpuProfileState QueryNvidiaProfile();                          // no admin needed
bool ConfigureNvidiaProfile(bool maxPerformance, std::wstring& message); // needs admin (saves driver settings)

} // namespace pd
