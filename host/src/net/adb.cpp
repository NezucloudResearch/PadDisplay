// PadDisplay by Nezucloud. Copyright (c) 2026 Nezucloud.
// SPDX-License-Identifier: MIT
#include "adb.h"
#include "log.h"
#include "childprocess.h"
#include "protocol.h"
#include "text.h"

#include <map>
#include <set>
#include <sstream>
#include <vector>

namespace pd
{
    namespace
    {

        constexpr DWORD kAdbTimeoutMs = 10000;
        constexpr DWORD kDevicePollMs = 2000;
        constexpr ULONGLONG kReapplyMs = 30000; // another adb client (e.g. scrcpy) may have restarted the server

        std::set<std::string> ReadyDevices(const std::wstring &adb)
        {
            std::istringstream lines(RunCapture(adb + L" devices", kAdbTimeoutMs));
            std::set<std::string> ready;
            std::string line;
            while (std::getline(lines, line))
            {
                auto tab = line.find('\t');
                if (tab == std::string::npos)
                    continue;
                std::string serial = line.substr(0, tab), state = line.substr(tab + 1);
                while (!state.empty() && (state.back() == '\r' || state.back() == ' '))
                    state.pop_back();
                if (state == "device")
                    ready.insert(serial);
                else if (state == "unauthorized")
                    LOGI("adb: %s is unauthorized - accept the USB debugging prompt on the tablet", serial.c_str());
            }
            return ready;
        }

    } // namespace

    std::wstring AdbReverser::FindAdb(const std::wstring &configured)
    {
        std::vector<std::wstring> candidates;
        if (!configured.empty())
            candidates.push_back(configured);
        candidates.push_back(L"C:\\android-platform-tools\\adb.exe");
        wchar_t local[MAX_PATH];
        if (GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH))
            candidates.push_back(std::wstring(local) + L"\\Android\\Sdk\\platform-tools\\adb.exe");
        wchar_t found[MAX_PATH];
        if (SearchPathW(nullptr, L"adb.exe", nullptr, MAX_PATH, found, nullptr))
            candidates.push_back(found);
        for (auto &c : candidates)
            if (GetFileAttributesW(c.c_str()) != INVALID_FILE_ATTRIBUTES)
                return c;
        return {};
    }

    void AdbReverser::Start(const std::wstring &adbPath)
    {
        adb_ = FindAdb(adbPath);
        if (adb_.empty())
        {
            LOGI("adb: not found; USB mode disabled (set adbPath in settings.ini)");
            return;
        }
        LOGI("adb: using %ls", adb_.c_str());
        stop_.Reset();
        thread_ = std::thread([this]
                              { Run(); });
    }

    void AdbReverser::Stop()
    {
        stop_.Request();
        if (thread_.joinable())
            thread_.join();
    }

    void AdbReverser::Run()
    {
        const std::wstring quoted = L"\"" + adb_ + L"\"";
        const std::wstring spec = L"tcp:" + std::to_wstring(kTcpPort);
        std::map<std::string, ULONGLONG> reversed; // serial -> when the reverse was last applied
        do
        {
            const std::set<std::string> ready = ReadyDevices(quoted);
            std::erase_if(reversed, [&](const auto &entry)
                          { return ready.count(entry.first) == 0; });
            const ULONGLONG now = GetTickCount64();
            for (const std::string &serial : ready)
            {
                auto it = reversed.find(serial);
                if (it != reversed.end() && now - it->second < kReapplyMs)
                    continue;
                DWORD code = 1;
                RunCapture(quoted + L" -s " + Widen(serial) + L" reverse " + spec + L" " + spec, kAdbTimeoutMs, &code);
                if (it == reversed.end() || code != 0)
                    LOGI("adb: reverse %s %s", serial.c_str(), code == 0 ? "ok" : "failed");
                if (code == 0)
                    reversed[serial] = now;
            }
            devices_ = int(ready.size());
        } while (!stop_.WaitFor(kDevicePollMs));
    }

} // namespace pd
