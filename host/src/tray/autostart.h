// PadDisplay by Nezucloud. Copyright (c) 2026 Nezucloud.
// SPDX-License-Identifier: MIT
// "Start with Windows": the Run key, or a scheduled task when PadDisplay has to start elevated.
#pragma once
#include "settings.h"

namespace pd
{

    bool RunKeyPresent();
    void SetRunKey(bool on);
    bool AutostartEnabled(const Settings &settings);
    bool CreateAutostartTask();
    bool DeleteAutostartTask();
    void SetAutostart(Settings &settings, bool on);

} // namespace pd
