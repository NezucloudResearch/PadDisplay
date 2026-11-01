// PadDisplay by Nezucloud. Copyright (c) 2026 Nezucloud.
// SPDX-License-Identifier: MIT
#pragma once
#include <cwchar>

namespace pd
{

    struct CommandLine
    {
        int argc = 0;
        wchar_t **argv = nullptr;

        bool Has(const wchar_t *name) const
        {
            for (int i = 1; i < argc; ++i)
                if (_wcsicmp(argv[i], name) == 0)
                    return true;
            return false;
        }

        const wchar_t *ValueOf(const wchar_t *name) const
        {
            for (int i = 1; i + 1 < argc; ++i)
                if (_wcsicmp(argv[i], name) == 0)
                    return argv[i + 1];
            return nullptr;
        }
    };

} // namespace pd
