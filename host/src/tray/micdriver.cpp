// PadDisplay by Nezucloud. Copyright (c) 2026 Nezucloud.
// SPDX-License-Identifier: MIT
#include "micdriver.h"

#include "log.h"
#include "mic/micbackend.h"
#include "setup/vdd.h"

#include <windows.h>

namespace pd
{

    // Switching the device off while an app records from it makes Windows' audio engine refuse; the device is then stuck "pending
    // restart" and comes back broken after it (tested). So the driver is switched off once nobody records from it: this waits, and
    // the wait ends when the user ticks the option again or PadDisplay exits. false = gave up (exiting, or a newer request).
    bool MicDriverSwitch::WaitForRecordersToStop(unsigned request, unsigned maxWaitMs)
    {
        const ULONGLONG start = GetTickCount64();
        bool told = false;
        int unknown = 0;
        for (;;)
        {
            const int n = MicRecorders();
            if (n == 0)
                return true;
            if (n < 0 && ++unknown > 10)
                return true; // held by something that is not recording (our own session closing, or a stuck program)
            if (request_ != request || GetTickCount64() - start >= maxWaitMs)
                return false;
            if (n > 0 && !told)
            {
                told = true;
                LOGI("mic driver: an app is recording from the microphone; switching the driver off when it stops");
            }
            Sleep(500); // nothing signals "the last recording stopped"; this only runs while the driver waits to be switched off
        }
    }

    void MicDriverSwitch::Apply(bool userAction)
    {
        const unsigned request = ++request_;
        std::lock_guard lock(mutex_);
        if (settings_.acceptTabletMic && ChooseMicBackend(settings_).backend == MicBackend::None)
        {
            if (userAction)
                offerCable_(); // ask whether to install VB-CABLE
            return;
        }
        const VddState installed = QueryMic();
        if (installed == VddState::NotInstalled || installed == VddState::Problem)
            return; // PadDisplay's driver is not what carries the mic here
        if (!settings_.acceptTabletMic && !WaitForRecordersToStop(request, INFINITE))
            return;
        if (SetMicDriverEnabled(settings_.acceptTabletMic, 8000))
        {
            needsRepair_ = false;
            return;
        }
        if (settings_.acceptTabletMic)
        { // e.g. a device left half-removed by an earlier restart: only the repair brings it back
            needsRepair_ = true;
            status_(L"Error: The microphone driver could not be switched on. In this menu choose \"Repair microphone driver (admin)\".");
        }
        else if (MicRestartPending())
            status_(L"Error: Windows could not switch the microphone driver off. Restart Windows to finish it.");
    }

    void MicDriverSwitch::DisableOnExit()
    {
        ++request_; // ends a wait for a recording app to stop
        std::lock_guard lock(mutex_);
        if (QueryMic() != VddState::Ready)
            return;
        if (MicRecorders() > 0)
        { // never hold up Exit by waiting; the next start switches it off
            LOGI("mic driver: left loaded, an app is still recording from it");
            return;
        }
        SetMicDriverEnabled(false, 4000);
    }

} // namespace pd
