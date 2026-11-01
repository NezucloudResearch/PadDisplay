// PadDisplay by Nezucloud. Copyright (c) 2026 Nezucloud.
// SPDX-License-Identifier: MIT
#pragma once
#include <windows.h>
#include <objbase.h>

namespace pd
{

    // COM for the length of one call, on whatever thread asks.
    struct ComForThisCall
    {
        const bool started = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
        ~ComForThisCall()
        {
            if (started)
                CoUninitialize();
        }
    };

} // namespace pd
