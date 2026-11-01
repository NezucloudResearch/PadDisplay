// PadDisplay by Nezucloud. Copyright (c) 2026 Nezucloud.
// SPDX-License-Identifier: MIT
#pragma once
#include <string>
#include <string_view>

namespace pd
{

    // UTF-16 to UTF-8 and back.
    std::string Narrow(const std::wstring &w);
    std::wstring Widen(std::string_view s);

} // namespace pd
