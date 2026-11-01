// PadDisplay by Nezucloud. Copyright (c) 2026 Nezucloud.
// SPDX-License-Identifier: MIT
#include "realtime.h"
#include "log.h"

#include <windows.h>
#include <avrt.h>

namespace pd
{
    namespace
    {

        // One per thread that sleeps, closed when that thread ends: sessions come and go, and their threads with them.
        struct ThreadTimer
        {
            HANDLE handle = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
            ~ThreadTimer()
            {
                if (handle)
                    CloseHandle(handle);
            }
        };

        // Task Scheduler starts a process with low I/O priority and memory priority 2 of 5, and it keeps them (measured): its memory
        // is trimmed first and its disk reads wait behind everything else. Puts both back to what a program from the Start menu has.
        void RestoreNormalIoAndMemoryPriority()
        {
            MEMORY_PRIORITY_INFORMATION memory{MEMORY_PRIORITY_NORMAL};
            SetProcessInformation(GetCurrentProcess(), ProcessMemoryPriority, &memory, sizeof(memory));
            using SetInformation = LONG(NTAPI *)(HANDLE, ULONG, PVOID, ULONG);
            const auto setInformation = reinterpret_cast<SetInformation>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtSetInformationProcess"));
            constexpr ULONG kProcessIoPriority = 33; // PROCESSINFOCLASS, no documented Win32 call sets it for a whole process
            ULONG normalIo = 2;                      // IoPriorityNormal
            if (setInformation)
                setInformation(GetCurrentProcess(), kProcessIoPriority, &normalIo, sizeof(normalIo));
        }

    } // namespace

    uint64_t NowUs()
    {
        static LARGE_INTEGER freq = []
        { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f; }();
        LARGE_INTEGER c;
        QueryPerformanceCounter(&c);
        return uint64_t(c.QuadPart / freq.QuadPart * 1000000 + (c.QuadPart % freq.QuadPart) * 1000000 / freq.QuadPart);
    }

    void SleepUs(uint64_t us)
    {
        thread_local ThreadTimer timer;
        if (!timer.handle)
        {
            Sleep(DWORD((us + 999) / 1000));
            return;
        }
        LARGE_INTEGER due;
        due.QuadPart = -LONGLONG(us * 10); // relative, 100 ns units
        if (SetWaitableTimer(timer.handle, &due, 0, nullptr, nullptr, FALSE))
            WaitForSingleObject(timer.handle, INFINITE);
    }

    void DisableProcessPowerThrottling()
    {
        PROCESS_POWER_THROTTLING_STATE s{};
        s.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
        s.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED | PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;
        s.StateMask = 0; // 0 = throttling off for the controlled aspects
        if (!SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &s, sizeof(s)))
            LOGI("power throttling opt-out failed (%lu)", GetLastError());
        SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS);
        RestoreNormalIoAndMemoryPriority();
    }

    void MakeRealtimeThread(const wchar_t *mmcssTask)
    {
        THREAD_POWER_THROTTLING_STATE t{};
        t.Version = THREAD_POWER_THROTTLING_CURRENT_VERSION;
        t.ControlMask = THREAD_POWER_THROTTLING_EXECUTION_SPEED;
        t.StateMask = 0;
        SetThreadInformation(GetCurrentThread(), ThreadPowerThrottling, &t, sizeof(t));
        DWORD index = 0;
        if (!AvSetMmThreadCharacteristicsW(mmcssTask, &index))
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    }

} // namespace pd
