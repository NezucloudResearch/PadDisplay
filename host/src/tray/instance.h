// PadDisplay by Nezucloud. Copyright (c) 2026 Nezucloud.
// SPDX-License-Identifier: MIT
// One PadDisplay at a time, and how one hands over to the next.
#pragma once
#include <windows.h>

namespace pd
{

    class SingleInstance
    {
    public:
        // false = another PadDisplay is running. While the previous one hands over, this waits for it (up to 20 s).
        bool Acquire();
        bool Held() const { return mutex_ != nullptr; }
        void Release();

    private:
        HANDLE mutex_ = nullptr;
    };

    // Asks Windows to start another PadDisplay (with or without administrator rights) and tells it to wait
    // for this one to finish (the marker file), because only one may run and they share port 27183.
    // false = it did not start (for example the UAC prompt was cancelled).
    bool StartOtherInstance(bool elevated);

    // Starts this program again with the same command line and the same rights. Call SingleInstance::Release first:
    // the new one takes the mutex at once, and no handover marker tells it to wait.
    bool RestartSameCommandLine();

} // namespace pd
