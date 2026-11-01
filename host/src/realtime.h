// PadDisplay by Nezucloud. Copyright (c) 2026 Nezucloud.
// SPDX-License-Identifier: MIT
// Time and scheduling for the threads where latency matters.
#pragma once
#include <cstdint>

namespace pd
{

    uint64_t NowUs();          // monotonic microseconds
    void SleepUs(uint64_t us); // sub-millisecond-accurate sleep (high-resolution waitable timer)

    // Windows 11 treats a window-less (tray) process as background after a few seconds and power-
    // throttles it (efficiency cores, coalesced timers): capture/encode latency rose from ~4 to ~14 ms.
    void DisableProcessPowerThrottling();
    // No throttling and high priority for this thread, with MMCSS scheduling under the given task class ("Capture", "Pro Audio", "Games").
    void MakeRealtimeThread(const wchar_t *mmcssTask);

} // namespace pd
