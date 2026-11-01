// PadDisplay by Nezucloud. Copyright (c) 2026 Nezucloud.
// SPDX-License-Identifier: MIT
// The tray (one administrator prompt) and the installer start PadDisplay.exe with one of these flags. Each needs administrator
// rights, does one thing, writes what happened to install-driver.log and exits with 0 when it worked.
#pragma once
#include "commandline.h"
#include "settings.h"

namespace pd
{

    struct SetupCommand
    {
        const wchar_t *flag;
        int (*run)(Settings &);
    };

    const SetupCommand *FindSetupCommand(const CommandLine &args);

} // namespace pd
