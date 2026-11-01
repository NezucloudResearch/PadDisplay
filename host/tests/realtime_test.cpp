// PadDisplay by Nezucloud. Copyright (c) 2026 Nezucloud.
// SPDX-License-Identifier: MIT
#include "realtime.h"

#include <windows.h>
#include <cstdio>
#include <thread>

using namespace pd;

namespace
{

    int failures = 0;

    void Expect(const char *test, bool passed)
    {
        if (passed)
            return;
        std::printf("FAILED %s\n", test);
        ++failures;
    }

    DWORD OpenHandles()
    {
        DWORD handles = 0;
        GetProcessHandleCount(GetCurrentProcess(), &handles);
        return handles;
    }

    void SleepOnANewThread()
    {
        std::thread([]
                    { SleepUs(100); })
            .join();
    }

    void AThreadThatSleptLeavesNoHandleBehind()
    {
        for (int i = 0; i < 10; ++i)
            SleepOnANewThread(); // whatever the first threads open once stays open
        const DWORD before = OpenHandles();
        for (int i = 0; i < 100; ++i)
            SleepOnANewThread();
        Expect("no timer per thread is left open", OpenHandles() < before + 10);
    }

    void NowUsNeverGoesBack()
    {
        uint64_t previous = NowUs();
        for (int i = 0; i < 100000; ++i)
        {
            const uint64_t now = NowUs();
            if (now < previous)
                Expect("the clock went back", false);
            previous = now;
        }
    }

    void SleepUsWaitsAtLeastAboutThatLong()
    {
        const uint64_t start = NowUs();
        SleepUs(2000);
        Expect("slept", NowUs() - start >= 1900);
    }

} // namespace

int main()
{
    AThreadThatSleptLeavesNoHandleBehind();
    NowUsNeverGoesBack();
    SleepUsWaitsAtLeastAboutThatLong();
    if (failures == 0)
        std::puts("realtime: all tests passed");
    return failures == 0 ? 0 : 1;
}
