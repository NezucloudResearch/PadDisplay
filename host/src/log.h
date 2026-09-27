// PadDisplay by Nezucloud. Copyright (c) 2026 Nezucloud.
// SPDX-License-Identifier: MIT
#pragma once
#include <cstdint>
#include <string>

namespace pd {

void LogInit(const std::wstring& path, bool console);
void Log(const char* fmt, ...);
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
