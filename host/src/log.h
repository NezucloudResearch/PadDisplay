// PadDisplay by Nezucloud. Copyright (c) 2026 Nezucloud.
// SPDX-License-Identifier: MIT
#pragma once
#include <cstdint>
#include <string>

namespace pd {

// append: keep what is in the file (the installer runs several setup commands in a row into one log; the tray host keeps
// the runs before, so a crash or a restart leaves the run that led to it). Appended files are rotated when they grow large.
void LogInit(const std::wstring& path, bool console, bool append = false);
void Log(const char* fmt, ...);
bool LogNoWait(const char* text); // for a crash handler: skips the line rather than wait for the lock
std::wstring LogPath();

std::string Narrow(const std::wstring& w);
std::wstring Widen(const std::string& s);
uint64_t NowUs(); // monotonic microseconds
void SleepUs(uint64_t us); // sub-millisecond-accurate sleep (high-resolution waitable timer)

// Windows 11 treats a window-less (tray) process as background after a few seconds and power-
// throttles it (efficiency cores, coalesced timers): capture/encode latency rose from ~4 to ~14 ms.
void DisableProcessPowerThrottling();
// For the capture/encode thread: no throttling, high priority, MMCSS "Capture" scheduling.
void MakeRealtimeThread(const wchar_t* mmcssTask);

} // namespace pd

#define LOGI(...) ::pd::Log(__VA_ARGS__)
