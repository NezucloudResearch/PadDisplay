// PadDisplay by Nezucloud. Copyright (c) 2026 Nezucloud.
// SPDX-License-Identifier: MIT
#pragma once
#include "stopevent.h"

#include <thread>

namespace pd
{

    // Broadcasts "PADDISPLAY <version> <tcpPort> <hostname>" on every IPv4 interface once a second.
    class DiscoveryBeacon
    {
    public:
        void Start();
        void Stop();

    private:
        void Run();
        StopEvent stop_;
        std::thread thread_;
    };

} // namespace pd
