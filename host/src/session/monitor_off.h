// PadDisplay by Nezucloud. Copyright (c) 2026 Nezucloud.
// SPDX-License-Identifier: MIT
#pragma once
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

namespace pd
{

    // Switches the virtual monitor off (or, without the on/off tasks, takes it off the desktop). Slow: seconds.
    void SwitchVirtualMonitorOff(const std::wstring &gdiName);

    // Switches the virtual monitor off a while after a session lost its tablet, unless a new session comes first: the windows on it
    // stay where they are for a tablet that reconnects. Waits on a condition variable (no polling) on its own thread. Its calls come
    // one after the other: the ending session's thread schedules, and whoever starts or stops the next session cancels or runs it.
    class DeferredMonitorOff
    {
    public:
        ~DeferredMonitorOff() { Cancel(); }
        void Schedule(int seconds, const std::wstring &gdiName);
        void Cancel(); // a new session uses the monitor as it is; waits if the switching is under way
        void RunNow(); // PadDisplay exits: switch it off now, if that was pending

    private:
        void Finish(bool switchOff);
        std::mutex mutex_;
        std::condition_variable wake_;
        bool cancel_ = false, now_ = false;
        std::thread thread_;
    };

} // namespace pd