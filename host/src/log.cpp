// PadDisplay by Nezucloud. Copyright (c) 2026 Nezucloud.
// SPDX-License-Identifier: MIT
#include "log.h"

#include <windows.h>
#include <cstdarg>
#include <cstdio>
#include <avrt.h>
#include <mutex>
#include <share.h>

namespace pd {

static std::mutex g_mutex;
static FILE* g_file = nullptr;
static bool g_console = false;
static std::wstring g_path;

void LogInit(const std::wstring& path, bool console) {
    g_path = path;
    g_console = console;
    // Reuse the launching terminal if there is one, otherwise open a console window.
    if (console && (AttachConsole(ATTACH_PARENT_PROCESS) || AllocConsole())) {
        FILE* f;
        freopen_s(&f, "CONOUT$", "w", stdout);
        SetConsoleOutputCP(CP_UTF8);
    }
    g_file = _wfsopen(path.c_str(), L"w", _SH_DENYNO); // readable while running
}

std::wstring LogPath() { return g_path; }

void Log(const char* fmt, ...) {
    char msg[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    SYSTEMTIME t;
    GetLocalTime(&t);
    char line[2200];
    snprintf(line, sizeof(line), "%02d:%02d:%02d.%03d %s\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, msg);

    std::lock_guard lock(g_mutex);
    OutputDebugStringA(line);
    if (g_console) fputs(line, stdout), fflush(stdout);
    if (g_file) fputs(line, g_file), fflush(g_file);
}

std::string Narrow(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring Widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
    return w;
}

void DisableProcessPowerThrottling() {
    PROCESS_POWER_THROTTLING_STATE s{};
    s.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    s.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED | PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;
    s.StateMask = 0; // 0 = throttling off for the controlled aspects
    if (!SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &s, sizeof(s)))
        Log("power throttling opt-out failed (%lu)", GetLastError());
    SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS);
}

void MakeRealtimeThread(const wchar_t* mmcssTask) {
    THREAD_POWER_THROTTLING_STATE t{};
    t.Version = THREAD_POWER_THROTTLING_CURRENT_VERSION;
    t.ControlMask = THREAD_POWER_THROTTLING_EXECUTION_SPEED;
    t.StateMask = 0;
    SetThreadInformation(GetCurrentThread(), ThreadPowerThrottling, &t, sizeof(t));
    DWORD index = 0;
    if (!AvSetMmThreadCharacteristicsW(mmcssTask, &index)) SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
}

void SleepUs(uint64_t us) {
    thread_local HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!timer) {
        Sleep(DWORD((us + 999) / 1000));
        return;
    }
    LARGE_INTEGER due;
    due.QuadPart = -LONGLONG(us * 10); // relative, 100 ns units
    if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) WaitForSingleObject(timer, INFINITE);
}

uint64_t NowUs() {
    static LARGE_INTEGER freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f; }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return uint64_t(c.QuadPart / freq.QuadPart * 1000000 + (c.QuadPart % freq.QuadPart) * 1000000 / freq.QuadPart);
}

} // namespace pd
