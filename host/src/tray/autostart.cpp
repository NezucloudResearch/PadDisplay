// PadDisplay by Nezucloud. Copyright (c) 2026 Nezucloud.
// SPDX-License-Identifier: MIT
#include "autostart.h"

#include "childprocess.h"
#include "display/securedesktop.h"
#include "exepath.h"
#include "log.h"
#include "setup/setup.h"
#include "setup/vdd.h"

#include <windows.h>
#include <sddl.h>
#include <shellapi.h>
#include <string>
#include <vector>

namespace pd
{
    namespace
    {

        constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
        constexpr wchar_t kAutostartTask[] = L"PadDisplay Autostart";

        std::wstring CurrentUserSid()
        {
            HANDLE token = nullptr;
            std::wstring sid;
            if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
            {
                DWORD n = 0;
                GetTokenInformation(token, TokenUser, nullptr, 0, &n);
                std::vector<BYTE> buf(n);
                wchar_t *str = nullptr;
                if (n && GetTokenInformation(token, TokenUser, buf.data(), n, &n) &&
                    ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER *>(buf.data())->User.Sid, &str))
                {
                    sid = str;
                    LocalFree(str);
                }
                CloseHandle(token);
            }
            return sid;
        }

        std::wstring AutostartTaskXml(const std::wstring &sid, const std::wstring &exe)
        {
            return L"<?xml version=\"1.0\" encoding=\"UTF-16\"?>\r\n"
                   L"<Task version=\"1.2\" xmlns=\"http://schemas.microsoft.com/windows/2004/02/mit/task\">\r\n"
                   L"  <RegistrationInfo><Author>PadDisplay</Author><Description>Starts PadDisplay with administrator rights when you sign in.</Description></RegistrationInfo>\r\n"
                   L"  <Triggers><LogonTrigger><Enabled>true</Enabled><UserId>" +
                   sid + L"</UserId></LogonTrigger></Triggers>\r\n"
                         L"  <Principals><Principal id=\"Author\"><UserId>" +
                   sid + L"</UserId><LogonType>InteractiveToken</LogonType><RunLevel>HighestAvailable</RunLevel></Principal></Principals>\r\n"
                         L"  <Settings>\r\n"
                         L"    <MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy>\r\n"
                         L"    <DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries>\r\n"
                         L"    <StopIfGoingOnBatteries>false</StopIfGoingOnBatteries>\r\n"
                         L"    <ExecutionTimeLimit>PT0S</ExecutionTimeLimit>\r\n"
                         L"    <AllowStartOnDemand>true</AllowStartOnDemand>\r\n"
                         L"  </Settings>\r\n"
                         L"  <Actions Context=\"Author\"><Exec><Command>" +
                   XmlEscape(exe) + L"</Command></Exec></Actions>\r\n"
                                    L"</Task>\r\n";
        }

        bool DeleteTaskWithUacPrompt(const std::wstring &args)
        {
            const std::wstring schtasks = SystemProgram(L"schtasks.exe");
            SHELLEXECUTEINFOW sei{sizeof(sei)};
            sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
            sei.lpVerb = L"runas";
            sei.lpFile = schtasks.c_str();
            sei.lpParameters = args.c_str();
            sei.nShow = SW_HIDE;
            DWORD code = 1;
            if (ShellExecuteExW(&sei) && sei.hProcess)
            {
                WaitForSingleObject(sei.hProcess, 15000);
                GetExitCodeProcess(sei.hProcess, &code);
                CloseHandle(sei.hProcess);
            }
            return code == 0;
        }

    } // namespace

    bool RunKeyPresent()
    {
        return RegGetValueW(HKEY_CURRENT_USER, kRunKey, L"PadDisplay", RRF_RT_REG_SZ, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
    }

    void SetRunKey(bool on)
    {
        HKEY key;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS)
            return;
        if (on)
        {
            const std::wstring cmd = L"\"" + ThisExePath() + L"\"";
            RegSetValueExW(key, L"PadDisplay", 0, REG_SZ, reinterpret_cast<const BYTE *>(cmd.c_str()), DWORD((cmd.size() + 1) * sizeof(wchar_t)));
        }
        else
        {
            RegDeleteValueW(key, L"PadDisplay");
        }
        RegCloseKey(key);
    }

    bool AutostartEnabled(const Settings &settings) { return RunKeyPresent() || settings.autostartTask; }

    // A Run-key program starts without administrator rights, and an elevated one would need a UAC prompt at
    // every sign-in. A scheduled task that runs with the highest rights starts it elevated, silently.
    // Needs this process to be elevated. Refused where a normal program could replace the exe (a portable copy in the user's
    // folders): the task would hand that program administrator rights at the next sign-in. Program Files is safe.
    bool CreateAutostartTask()
    {
        const std::wstring exe = ThisExePath();
        if (!OnlyAdminsCanChange(exe))
        {
            LOGI("autostart: no administrator start at sign-in from %ls, because programs without administrator rights can change it. "
                 "Install PadDisplay with its installer (into Program Files) to get it.",
                 exe.c_str());
            return false;
        }
        const std::wstring sid = CurrentUserSid();
        if (sid.empty())
            return false;
        std::wstring message;
        return CreateTaskFromXml(kAutostartTask, AutostartTaskXml(sid, exe), message);
    }

    // Removing the task needs elevation too: from a normal process, ask for it (one UAC prompt).
    bool DeleteAutostartTask()
    {
        const std::wstring args = L"/delete /tn \"" + std::wstring(kAutostartTask) + L"\" /f";
        if (!HostIsElevated())
            return DeleteTaskWithUacPrompt(args);
        DWORD code = 1;
        RunCapture(SchtasksCommand() + args, 15000, &code);
        return code == 0;
    }

    void SetAutostart(Settings &settings, bool on)
    {
        if (on && HostIsElevated() && settings.runAsAdmin && CreateAutostartTask())
        {
            settings.autostartTask = true;
            SetRunKey(false);
        }
        else if (on)
        {
            // Not elevated, or the task could not be made: the plain Run key (starts without admin rights).
            if (settings.autostartTask && DeleteAutostartTask())
                settings.autostartTask = false;
            SetRunKey(true);
        }
        else
        {
            SetRunKey(false);
            if (settings.autostartTask && DeleteAutostartTask())
                settings.autostartTask = false;
        }
        settings.Save();
    }

} // namespace pd
