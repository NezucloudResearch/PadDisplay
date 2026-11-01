// PadDisplay by Nezucloud. Copyright (c) 2026 Nezucloud.
// SPDX-License-Identifier: MIT
#include "monitor_off.h"
#include "display/display.h"
#include "display/realscreens.h"
#include "log.h"
#include "setup/vdd.h"

#include <chrono>

namespace pd
{
    // Nobody is using it any more: switch the virtual monitor off, so Windows shows only the real screens and the user's layout is
    // back as they had it (SetVirtualMonitorEnabled restores it). Without the on/off tasks (the admin setup was never run) the device
    // cannot be switched, so at least take it off the desktop.
    void SwitchVirtualMonitorOff(const std::wstring &gdiName)
    {
        if (SetVirtualMonitorEnabled(false, 5000))
            return;
        Detach(gdiName);
        RestoreRealScreens(); // detaching can make Windows shuffle the real screens
    }

    void DeferredMonitorOff::Schedule(int seconds, const std::wstring &gdiName)
    {
        Cancel();
        {
            std::lock_guard lock(mutex_);
            cancel_ = now_ = false;
        }
        thread_ = std::thread([this, seconds, gdiName]
                              {
        std::unique_lock lock(mutex_);
        wake_.wait_for(lock, std::chrono::seconds(seconds), [this] { return cancel_ || now_; });
        const bool cancelled = cancel_;
        lock.unlock();
        if (cancelled) return;
        LOGI("session: the tablet did not come back, switching the virtual monitor off");
        SwitchVirtualMonitorOff(gdiName); });
    }

    void DeferredMonitorOff::Cancel() { Finish(false); }

    void DeferredMonitorOff::RunNow() { Finish(true); }

    void DeferredMonitorOff::Finish(bool switchOff)
    {
        if (!thread_.joinable())
            return;
        {
            std::lock_guard lock(mutex_);
            (switchOff ? now_ : cancel_) = true;
        }
        wake_.notify_all();
        thread_.join();
    }

} // namespace pd
