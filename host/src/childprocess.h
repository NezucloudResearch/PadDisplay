// PadDisplay by Nezucloud. Copyright (c) 2026 Nezucloud.
// SPDX-License-Identifier: MIT
#pragma once
#include <windows.h>
#include <string>

namespace pd
{

    // Runs a console program hidden and returns its stdout (empty on failure).
    std::string RunCapture(const std::wstring &cmdline, DWORD timeoutMs, DWORD *exitCode = nullptr);

} // namespace pd
