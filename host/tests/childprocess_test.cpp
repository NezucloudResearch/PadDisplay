// PadDisplay by Nezucloud. Copyright (c) 2026 Nezucloud.
// SPDX-License-Identifier: MIT
#include "childprocess.h"

#include <cstdio>
#include <string>

using pd::RunCapture;

namespace
{

    int failures = 0;

    constexpr DWORD kPlenty = 10000;

    void Expect(const char *test, bool passed)
    {
        if (passed)
            return;
        std::printf("FAILED %s\n", test);
        ++failures;
    }

    std::wstring Cmd(const std::wstring &line)
    {
        wchar_t system[MAX_PATH];
        GetSystemDirectoryW(system, MAX_PATH);
        return L"\"" + std::wstring(system) + L"\\cmd.exe\" /c " + line;
    }

    void ItReturnsWhatTheProgramPrinted()
    {
        Expect("stdout", RunCapture(Cmd(L"echo hello"), kPlenty).find("hello") != std::string::npos);
    }

    void ItReturnsErrorOutputToo()
    {
        Expect("stderr", RunCapture(Cmd(L"echo oops 1>&2"), kPlenty).find("oops") != std::string::npos);
    }

    void ItReportsTheExitCode()
    {
        DWORD code = 99;
        RunCapture(Cmd(L"exit 3"), kPlenty, &code);
        Expect("exit code", code == 3);
    }

    void ItReadsMoreThanThePipeHolds()
    {
        const std::string out = RunCapture(Cmd(L"for /l %i in (1,1,3000) do @echo a line that is long enough to fill the pipe's buffer"), kPlenty * 3);
        Expect("all the lines", out.size() > 150000);
    }

    void AProgramThatDoesNotExistGivesNothing()
    {
        DWORD code = 99;
        Expect("no output", RunCapture(L"C:\\no\\such\\program.exe", kPlenty, &code).empty());
        Expect("no exit code", code == 99);
    }

    void AProgramThatRunsTooLongIsStopped()
    {
        const ULONGLONG start = GetTickCount64();
        DWORD code = 0;
        RunCapture(Cmd(L"ping -n 20 127.0.0.1"), 300, &code);
        Expect("it did not wait for the program", GetTickCount64() - start < 5000);
        Expect("it was terminated", code == 1);
    }

    void ItLeavesNoHandlesBehind()
    {
        const auto handles = []
        {
            DWORD count = 0;
            GetProcessHandleCount(GetCurrentProcess(), &count);
            return count;
        };
        RunCapture(Cmd(L"exit 0"), kPlenty);
        const DWORD before = handles();
        for (int i = 0; i < 20; ++i)
            RunCapture(Cmd(L"exit 0"), kPlenty);
        Expect("no handle per run", handles() < before + 5);
    }

} // namespace

int main()
{
    ItReturnsWhatTheProgramPrinted();
    ItReturnsErrorOutputToo();
    ItReportsTheExitCode();
    ItReadsMoreThanThePipeHolds();
    AProgramThatDoesNotExistGivesNothing();
    AProgramThatRunsTooLongIsStopped();
    ItLeavesNoHandlesBehind();
    if (failures == 0)
        std::puts("childprocess: all tests passed");
    return failures == 0 ? 0 : 1;
}
