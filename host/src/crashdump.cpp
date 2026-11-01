// PadDisplay by Nezucloud. Copyright (c) 2026 Nezucloud.
// SPDX-License-Identifier: MIT
#include "crashdump.h"
#include "log.h"

#include <windows.h>
#include <dbghelp.h>
#include <csignal>

namespace pd {

static std::wstring g_dumpPath;
static volatile LONG g_written = 0;

static void WriteDump(EXCEPTION_POINTERS* exception) {
    if (InterlockedExchange(&g_written, 1)) return; // once: a second crash while writing must not start over
    HANDLE file = CreateFileW(g_dumpPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    MINIDUMP_EXCEPTION_INFORMATION info{GetCurrentThreadId(), exception, FALSE};
    const bool ok = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file, MiniDumpNormal, exception ? &info : nullptr,
                                      nullptr, nullptr) != FALSE;
    CloseHandle(file);
    // Not LOGI: the crash may have happened while the log's lock was held, and waiting for it would hang the process.
    LogNoWait(ok ? "crash: wrote crash.dmp next to this log" : "crash: could not write crash.dmp");
}

static LONG WINAPI OnUnhandledException(EXCEPTION_POINTERS* exception) {
    WriteDump(exception);
    return EXCEPTION_CONTINUE_SEARCH;
}

// abort(), which is also how an uncaught C++ exception ends the process (std::terminate), on whichever thread.
static void OnAbort(int) { WriteDump(nullptr); }

void InstallCrashHandler(const std::wstring& dir) {
    g_dumpPath = dir + L"\\crash.dmp";
    SetUnhandledExceptionFilter(OnUnhandledException);
    signal(SIGABRT, OnAbort);
}

} // namespace pd
