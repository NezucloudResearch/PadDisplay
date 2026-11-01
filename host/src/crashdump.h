// PadDisplay by Nezucloud. Copyright (c) 2026 Nezucloud.
// SPDX-License-Identifier: MIT
#pragma once
#include <string>

namespace pd
{

    // When the process crashes (an access violation, or an uncaught C++ exception on any thread), writes <dir>\crash.dmp with the
    // threads' stacks only (no heap) and a line into the log, then lets Windows Error Reporting carry on. For the tray host only:
    // the SYSTEM processes write theirs nowhere a user could read (see SECURITY.md).
    void InstallCrashHandler(const std::wstring &dir);

} // namespace pd
