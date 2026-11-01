// PadDisplay by Nezucloud. Copyright (c) 2026 Nezucloud.
// SPDX-License-Identifier: MIT
// The PadDisplay Microphone driver is loaded only while PadDisplay runs with "Accept the tablet's microphone" ticked: the
// device is enabled then and disabled otherwise (it is a kernel driver that adds a recording device, so it is not left on).
// Worker threads only: switching waits for a scheduled task.
#pragma once
#include "settings.h"

#include <atomic>
#include <functional>
#include <mutex>
#include <string>

namespace pd
{

    class MicDriverSwitch
    {
    public:
        using StatusFn = std::function<void(const std::wstring &)>;
        using OfferCableFn = std::function<void()>;
        MicDriverSwitch(const Settings &settings, StatusFn status, OfferCableFn offerCable)
            : settings_(settings), status_(std::move(status)), offerCable_(std::move(offerCable)) {}

        // userAction: the user just ticked or unticked the option (not the start-up pass), so a missing microphone may be offered.
        void Apply(bool userAction);
        void DisableOnExit();
        bool NeedsRepair() const { return needsRepair_; }
        void MarkRepaired() { needsRepair_ = false; }

    private:
        bool WaitForRecordersToStop(unsigned request, unsigned maxWaitMs);

        const Settings &settings_;
        StatusFn status_;
        OfferCableFn offerCable_;
        std::mutex mutex_;
        std::atomic<unsigned> request_{0};     // a newer request ends an older one that is still waiting for an app to stop recording
        std::atomic<bool> needsRepair_{false}; // switching the device on failed: the tray offers the repair
    };

} // namespace pd
