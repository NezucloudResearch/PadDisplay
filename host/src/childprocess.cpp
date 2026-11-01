// PadDisplay by Nezucloud. Copyright (c) 2026 Nezucloud.
// SPDX-License-Identifier: MIT
#include "childprocess.h"

#include <algorithm>
#include <vector>

namespace pd
{
    namespace
    {

        // Only the pipe's write end is inherited. Inheriting every inheritable handle leaked the listening socket into the adb server
        // that `adb devices` starts and leaves running: after Exit the port stayed bound, and the accept thread could not be woken by
        // closing the socket, so Exit hung.
        bool StartWithOnlyThePipe(std::wstring cmdline, HANDLE pipeWrite, PROCESS_INFORMATION &process)
        {
            STARTUPINFOEXW si{};
            si.StartupInfo.cb = sizeof(si);
            si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
            si.StartupInfo.hStdOutput = si.StartupInfo.hStdError = pipeWrite;
            SIZE_T attrSize = 0;
            InitializeProcThreadAttributeList(nullptr, 1, 0, &attrSize);
            std::vector<uint8_t> attrBuf(attrSize);
            si.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attrBuf.data());
            if (!InitializeProcThreadAttributeList(si.lpAttributeList, 1, 0, &attrSize))
                return false;
            const bool started = UpdateProcThreadAttribute(si.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, &pipeWrite, sizeof(pipeWrite), nullptr, nullptr) &&
                                 CreateProcessW(nullptr, cmdline.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr, &si.StartupInfo, &process);
            DeleteProcThreadAttributeList(si.lpAttributeList);
            return started;
        }

        // Polls instead of reading to EOF: a spawned adb server daemon inherits the pipe and keeps it open.
        std::string ReadUntilExit(HANDLE pipeRead, HANDLE process, DWORD timeoutMs)
        {
            constexpr DWORD kPollMs = 20;
            std::string out;
            char buf[4096];
            const ULONGLONG deadline = GetTickCount64() + timeoutMs;
            for (;;)
            {
                DWORD avail = 0;
                if (!PeekNamedPipe(pipeRead, nullptr, 0, nullptr, &avail, nullptr))
                    break; // writer closed
                if (avail > 0)
                {
                    DWORD got = 0;
                    if (!ReadFile(pipeRead, buf, std::min<DWORD>(avail, sizeof(buf)), &got, nullptr) || got == 0)
                        break;
                    out.append(buf, got);
                    continue;
                }
                if (WaitForSingleObject(process, kPollMs) == WAIT_OBJECT_0)
                {
                    if (PeekNamedPipe(pipeRead, nullptr, 0, nullptr, &avail, nullptr) && avail > 0)
                        continue;
                    break;
                }
                if (GetTickCount64() > deadline)
                {
                    TerminateProcess(process, 1);
                    break;
                }
            }
            return out;
        }

    } // namespace

    std::string RunCapture(const std::wstring &cmdline, DWORD timeoutMs, DWORD *exitCode)
    {
        SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
        HANDLE pipeRead = nullptr, pipeWrite = nullptr;
        if (!CreatePipe(&pipeRead, &pipeWrite, &sa, 0))
            return {};
        SetHandleInformation(pipeRead, HANDLE_FLAG_INHERIT, 0);
        PROCESS_INFORMATION process{};
        const bool started = StartWithOnlyThePipe(cmdline, pipeWrite, process);
        CloseHandle(pipeWrite);
        std::string out;
        if (started)
        {
            out = ReadUntilExit(pipeRead, process.hProcess, timeoutMs);
            WaitForSingleObject(process.hProcess, 1000);
            if (exitCode)
                GetExitCodeProcess(process.hProcess, exitCode);
            CloseHandle(process.hThread);
            CloseHandle(process.hProcess);
        }
        CloseHandle(pipeRead);
        return out;
    }

} // namespace pd
