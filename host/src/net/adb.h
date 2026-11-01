// PadDisplay by Nezucloud. Copyright (c) 2026 Nezucloud.
// SPDX-License-Identifier: MIT
#pragma once
#include "stopevent.h"

#include <atomic>
#include <string>
#include <thread>

namespace pd
{

    // Keeps `adb reverse tcp:PORT tcp:PORT` applied on every attached device so the tablet can reach
    // the host at 127.0.0.1 over USB.
    class AdbReverser
    {
    public:
        void Start(const std::wstring &adbPath);
        void Stop();
        int DeviceCount() const { return devices_; }
        static std::wstring FindAdb(const std::wstring &configured);

    private:
        void Run();
        std::wstring adb_;
        StopEvent stop_;
        std::atomic<int> devices_{0};
        std::thread thread_;
    };

} // namespace pd
