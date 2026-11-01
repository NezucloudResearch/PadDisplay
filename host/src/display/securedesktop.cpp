// PadDisplay by Nezucloud. Copyright (c) 2026 Nezucloud.
// SPDX-License-Identifier: MIT
#include "securedesktop.h"
#include "log.h"
#include "settings.h"

#include <windows.h>
#include <atomic>
#include <cstdio>

namespace pd
{
    // --- UAC prompts the tablet can click, in only-screen mode -----------------------------------
    // A UAC prompt normally appears on the secure desktop, which Desktop Duplication cannot capture. In
    // only-screen mode the PC's real screens are off, so the prompt is invisible and unanswerable.
    // Setting PromptOnSecureDesktop = 0 moves prompts to the normal desktop: the tablet then shows them,
    // and - because an elevated host injects input at the same (high) integrity as the consent dialog -
    // can click them. The value is machine-wide, so it is changed only while a tablet is the only screen
    // and restored the moment that ends. The old value goes to a marker file first, so a crash is
    // recoverable (RestoreSecureDesktop runs at start-up too).
    namespace
    {
        constexpr wchar_t kUacPolicyKey[] = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Policies\\System";
        constexpr wchar_t kUacPolicyValue[] = L"PromptOnSecureDesktop";

        std::wstring SecureDesktopMarker() { return AppDataDir() + L"\\secure-desktop.txt"; }

        bool ReadUacPolicy(DWORD &value)
        {
            DWORD size = sizeof(value);
            LONG r = RegGetValueW(HKEY_LOCAL_MACHINE, kUacPolicyKey, kUacPolicyValue, RRF_RT_REG_DWORD, nullptr, &value, &size);
            if (r == ERROR_FILE_NOT_FOUND)
            {
                value = 1;
                return true;
            } // absent on a default install = secure desktop on
            return r == ERROR_SUCCESS;
        }

        bool WriteUacPolicy(DWORD value)
        {
            return RegSetKeyValueW(HKEY_LOCAL_MACHINE, kUacPolicyKey, kUacPolicyValue, REG_DWORD, &value, sizeof(value)) == ERROR_SUCCESS;
        }
    } // namespace

    bool HostIsElevated()
    {
        HANDLE token = nullptr;
        TOKEN_ELEVATION e{};
        DWORD n = 0;
        bool elevated = OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token) &&
                        GetTokenInformation(token, TokenElevation, &e, sizeof(e), &n) && e.TokenIsElevated;
        if (token)
            CloseHandle(token);
        return elevated;
    }

    // Set from the tray message thread, read from the session thread: a plain atomic is enough (no
    // ordering with other state), and it never blocks a hot thread.
    namespace
    {
        std::atomic<bool> g_sessionLocked{false};
    }
    void SetSessionLocked(bool locked) { g_sessionLocked.store(locked, std::memory_order_relaxed); }
    bool SessionIsLocked() { return g_sessionLocked.load(std::memory_order_relaxed); }

    bool AllowUacClicksOnTablet()
    {
        if (!HostIsElevated())
        { // writing HKLM policy, and clicking the elevated prompt, both need elevation
            LOGI("uac: not moving prompts off the secure desktop - PadDisplay is not elevated");
            return false;
        }
        DWORD current = 1;
        if (!ReadUacPolicy(current))
        {
            LOGI("uac: could not read PromptOnSecureDesktop");
            return false;
        }
        if (current == 0)
            return true; // already on the normal desktop: nothing to change, nothing to undo
        // Record the old value before touching the policy, so a crash cannot strand the secure desktop off. No record, no change.
        FILE *f = nullptr;
        if (_wfopen_s(&f, SecureDesktopMarker().c_str(), L"w") || !f)
        {
            LOGI("uac: not moving prompts off the secure desktop - cannot record how to put it back");
            return false;
        }
        fwprintf(f, L"%lu", current);
        fclose(f);
        if (!WriteUacPolicy(0))
        {
            LOGI("uac: could not move prompts off the secure desktop");
            DeleteFileW(SecureDesktopMarker().c_str());
            return false;
        }
        LOGI("uac: prompts moved to the normal desktop so the tablet can show and click them (was %lu)", current);
        return true;
    }

    void RestoreSecureDesktop()
    {
        const std::wstring marker = SecureDesktopMarker();
        FILE *f = nullptr;
        if (_wfopen_s(&f, marker.c_str(), L"r") || !f)
            return; // nothing was changed
        unsigned long saved = 1;
        if (fwscanf_s(f, L"%lu", &saved) != 1)
            saved = 1;
        fclose(f);
        if (!HostIsElevated())
        { // keep the marker: a later elevated run will put the policy back
            LOGI("uac: secure desktop should be restored to %lu but PadDisplay is not elevated; will retry when elevated", saved);
            return;
        }
        if (WriteUacPolicy(DWORD(saved)))
        {
            LOGI("uac: secure desktop restored (PromptOnSecureDesktop = %lu)", saved);
            DeleteFileW(marker.c_str());
        }
        else
        {
            LOGI("uac: could not restore PromptOnSecureDesktop to %lu (marker kept)", saved);
        }
    }

} // namespace pd
