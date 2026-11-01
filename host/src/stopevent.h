// PadDisplay by Nezucloud. Copyright (c) 2026 Nezucloud.
// SPDX-License-Identifier: MIT
#pragma once
#include <windows.h>

namespace pd
{

    // A stop request a background loop can sleep on. It ends the wait at once, and the loop costs no wakeups in between, where
    // sleeping in short steps to look at a flag wakes the process ten times a second for nothing.
    class StopEvent
    {
    public:
        StopEvent() = default;
        ~StopEvent() { CloseHandle(event_); }
        StopEvent(const StopEvent &) = delete;
        StopEvent &operator=(const StopEvent &) = delete;

        void Reset() { ResetEvent(event_); }
        void Request() { SetEvent(event_); }
        // Sleeps for up to ms. true = stop was requested.
        bool WaitFor(DWORD ms) const { return WaitForSingleObject(event_, ms) == WAIT_OBJECT_0; }
        HANDLE Handle() const { return event_; } // for a loop that waits on other things as well

    private:
        HANDLE event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    };

} // namespace pd
