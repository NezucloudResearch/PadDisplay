// PadDisplay by Nezucloud. Copyright (c) 2026 Nezucloud.
// SPDX-License-Identifier: MIT
#pragma once
#include <string>

namespace pd
{

    // append: keep what is in the file (the installer runs several setup commands in a row into one log; the tray host keeps
    // the runs before, so a crash or a restart leaves the run that led to it). Appended files are rotated when they grow large.
    void LogInit(const std::wstring &path, bool console, bool append = false);
    void Log(const char *fmt, ...);
    bool LogNoWait(const char *text); // for a crash handler: skips the line rather than wait for the lock
    std::wstring LogPath();

} // namespace pd

#define LOGI(...) ::pd::Log(__VA_ARGS__)
