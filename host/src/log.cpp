// PadDisplay by Nezucloud. Copyright (c) 2026 Nezucloud.
// SPDX-License-Identifier: MIT
#include "log.h"

#include <windows.h>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <share.h>

namespace pd
{

    static std::mutex g_mutex;
    static FILE *g_file = nullptr;
    static bool g_console = false;
    static std::wstring g_path;

    // A log kept across runs moves to "<name>.old.log" once it is this big, so the run before a crash is still there and the
    // files do not grow for ever.
    static void RotateIfLarge(const std::wstring &path)
    {
        constexpr ULONGLONG kMaxBytes = 2 * 1024 * 1024;
        WIN32_FILE_ATTRIBUTE_DATA info{};
        if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &info))
            return;
        if ((ULONGLONG(info.nFileSizeHigh) << 32 | info.nFileSizeLow) < kMaxBytes)
            return;
        const size_t dot = path.rfind(L'.');
        const std::wstring old = (dot == std::wstring::npos ? path : path.substr(0, dot)) + L".old.log";
        MoveFileExW(path.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING);
    }

    static void WriteOut(FILE *file, const char *text)
    {
        fputs(text, file);
        fflush(file);
    }

    void LogInit(const std::wstring &path, bool console, bool append)
    {
        g_path = path;
        g_console = console;
        // Reuse the launching terminal if there is one, otherwise open a console window.
        if (console && (AttachConsole(ATTACH_PARENT_PROCESS) || AllocConsole()))
        {
            FILE *f;
            freopen_s(&f, "CONOUT$", "w", stdout);
            SetConsoleOutputCP(CP_UTF8);
        }
        if (append)
            RotateIfLarge(path);
        g_file = _wfsopen(path.c_str(), append ? L"a" : L"w", _SH_DENYNO); // readable while running
        // The lines carry the time of day only: this says which day, and where one run ends and the next begins.
        SYSTEMTIME t;
        GetLocalTime(&t);
        Log("---- %04d-%02d-%02d, process %lu", t.wYear, t.wMonth, t.wDay, GetCurrentProcessId());
    }

    std::wstring LogPath() { return g_path; }

    void Log(const char *fmt, ...)
    {
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
        if (g_console)
            WriteOut(stdout, line);
        if (g_file)
            WriteOut(g_file, line);
    }

    bool LogNoWait(const char *text)
    {
        std::unique_lock lock(g_mutex, std::try_to_lock);
        if (!lock.owns_lock() || !g_file)
            return false;
        fputs(text, g_file);
        WriteOut(g_file, "\n");
        return true;
    }

} // namespace pd
