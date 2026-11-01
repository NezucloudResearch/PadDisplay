// PadDisplay by Nezucloud. Copyright (c) 2026 Nezucloud.
// SPDX-License-Identifier: MIT
// Which GPU Windows runs this program on. It reads the preference when a program starts, so a change needs one restart.
#pragma once
#include "settings.h"

#include <string>

namespace pd
{

    // Writes the preference for this exe if it is not the wanted one. true = it changed, so a restart is needed to apply it.
    bool EnsureGpuPreference(const Settings &settings, const std::wstring &gdiName);

    // Has the GPU preference for this exe been written before? It only changes if the virtual monitor moves to another GPU (a
    // different "gpu" in vdd_settings.xml), so the monitor does not have to be on at every start just to find out. To have it
    // detected again after such a change, delete this exe's value under HKCU\Software\Microsoft\DirectX\UserGpuPreferences.
    bool GpuPreferenceStored();

    // The uninstaller: leaves no value behind.
    void ForgetGpuPreference();

} // namespace pd
