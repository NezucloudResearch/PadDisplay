// PadDisplay by Nezucloud. Copyright (c) 2026 Nezucloud.
// SPDX-License-Identifier: MIT
#include "instance.h"

#include "exepath.h"
#include "settings.h"

#include <shellapi.h>
#include <cstdio>
#include <string>

namespace pd
{
    namespace
    {

        std::wstring HandoverMarkerPath() { return AppDataDir() + L"\\handover"; }

        // A fresh handover marker means the previous PadDisplay is on its way out: wait for it.
        bool HandoverPending()
        {
            WIN32_FILE_ATTRIBUTE_DATA a{};
            if (!GetFileAttributesExW(HandoverMarkerPath().c_str(), GetFileExInfoStandard, &a))
                return false;
            FILETIME now;
            GetSystemTimeAsFileTime(&now);
            ULARGE_INTEGER t{}, n{};
            t.LowPart = a.ftLastWriteTime.dwLowDateTime;
            t.HighPart = a.ftLastWriteTime.dwHighDateTime;
            n.LowPart = now.dwLowDateTime;
            n.HighPart = now.dwHighDateTime;
            return n.QuadPart >= t.QuadPart && n.QuadPart - t.QuadPart < 60ull * 10000000;
        }

    } // namespace

    // A mutex made by an elevated process cannot be opened by a normal one (access denied), which also means that one is running.
    bool SingleInstance::Acquire()
    {
        for (int attempt = 0;; ++attempt)
        {
            HANDLE h = CreateMutexW(nullptr, TRUE, L"Local\\PadDisplayHost");
            const bool running = h ? GetLastError() == ERROR_ALREADY_EXISTS : GetLastError() == ERROR_ACCESS_DENIED;
            if (!running)
            {
                DeleteFileW(HandoverMarkerPath().c_str());
                mutex_ = h;
                return true;
            }
            if (h)
                CloseHandle(h);
            if (attempt >= 100 || !HandoverPending())
                return false;
            Sleep(200);
        }
    }

    void SingleInstance::Release()
    {
        if (!mutex_)
            return;
        ReleaseMutex(mutex_);
        CloseHandle(mutex_);
        mutex_ = nullptr;
    }

    bool StartOtherInstance(bool elevated)
    {
        const std::wstring marker = HandoverMarkerPath();
        if (FILE *f = nullptr; !_wfopen_s(&f, marker.c_str(), L"w") && f)
            fclose(f);
        const std::wstring exe = ThisExePath();
        const std::wstring quoted = L"\"" + exe + L"\"";
        SHELLEXECUTEINFOW sei{sizeof(sei)};
        sei.nShow = SW_SHOWNORMAL;
        if (elevated)
        {
            sei.lpVerb = L"runas";
            sei.lpFile = exe.c_str();
        }
        else
        { // explorer.exe starts it with the desktop's normal rights, not ours
            sei.lpFile = L"explorer.exe";
            sei.lpParameters = quoted.c_str();
        }
        const bool ok = ShellExecuteExW(&sei) != FALSE;
        if (!ok)
            DeleteFileW(marker.c_str());
        return ok;
    }

    bool RestartSameCommandLine()
    {
        STARTUPINFOW si{sizeof(si)};
        PROCESS_INFORMATION pi{};
        std::wstring cmd = GetCommandLineW();
        if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi))
            return false;
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return true;
    }

} // namespace pd
