// PadDisplay by Nezucloud. Copyright (c) 2026 Nezucloud.
// SPDX-License-Identifier: MIT
#include "exepath.h"

#include <windows.h>

namespace pd
{

    std::wstring ThisExePath()
    {
        wchar_t exe[MAX_PATH];
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        return exe;
    }

    std::wstring ThisExeDir()
    {
        const std::wstring path = ThisExePath();
        return path.substr(0, path.find_last_of(L'\\'));
    }

} // namespace pd
