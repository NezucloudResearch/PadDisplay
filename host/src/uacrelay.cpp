// PadDisplay by Nezucloud. Copyright (c) 2026 Nezucloud.
// SPDX-License-Identifier: MIT
#include "uacrelay.h"
#include "log.h"
#include "net.h"      // RunCapture
#include "vdd.h"      // CreateTaskFromXml
#include "settings.h" // AppDataDir

#include <windows.h>
#include <wtsapi32.h>
#include <sddl.h>
#include <userenv.h>
#include <algorithm>
#include <array>

namespace pd {
namespace {

// The executable basename of the foreground window's process ("?" if it cannot be read). Logged on an
// ignored command so a window that is not consent.exe (a differently named secure UI) can be spotted.
std::wstring ForegroundImage() {
    HWND fg = GetForegroundWindow();
    if (!fg) return L"<none>";
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    if (!pid) return L"<no-pid>";
    HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!proc) return L"<open-denied>"; // our System token should open consent.exe; if not, that is the clue
    wchar_t path[MAX_PATH] = {};
    DWORD n = MAX_PATH;
    bool ok = QueryFullProcessImageNameW(proc, 0, path, &n) != 0;
    CloseHandle(proc);
    if (!ok) return L"<query-failed>";
    const wchar_t* slash = wcsrchr(path, L'\\');
    return slash ? slash + 1 : path;
}

// The consent prompt is drawn by consent.exe. Inject only when it is the foreground window.
bool ForegroundIsConsent() { return _wcsicmp(ForegroundImage().c_str(), L"consent.exe") == 0; }

// Only these keys are forwarded to a UAC prompt. Anything else is dropped, so the channel cannot be
// used to type arbitrary input as System.
bool KeyAllowed(WORD vk) {
    switch (vk) {
    case VK_RETURN:
    case VK_ESCAPE:
    case VK_TAB:
    case VK_LEFT:
    case VK_RIGHT:
    case VK_UP:
    case VK_DOWN:
    case VK_SPACE:
        return true;
    default:
        return false;
    }
}

void InjectClick(int x, int y) {
    int vx = GetSystemMetrics(SM_XVIRTUALSCREEN), vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int vw = (std::max)(2, GetSystemMetrics(SM_CXVIRTUALSCREEN)), vh = (std::max)(2, GetSystemMetrics(SM_CYVIRTUALSCREEN));
    std::array<INPUT, 3> in{};
    const LONG nx = LONG((int64_t(x - vx) * 65535 + (vw - 1) / 2) / (vw - 1));
    const LONG ny = LONG((int64_t(y - vy) * 65535 + (vh - 1) / 2) / (vh - 1));
    for (auto& i : in) {
        i.type = INPUT_MOUSE;
        i.mi.dx = nx;
        i.mi.dy = ny;
        i.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
    }
    in[1].mi.dwFlags |= MOUSEEVENTF_LEFTDOWN;
    in[2].mi.dwFlags |= MOUSEEVENTF_LEFTUP;
    SendInput(UINT(in.size()), in.data(), sizeof(INPUT));
}

void InjectKey(WORD vk) {
    INPUT in[2] = {};
    in[0].type = in[1].type = INPUT_KEYBOARD;
    in[0].ki.wVk = in[1].ki.wVk = vk;
    in[1].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(2, in, sizeof(INPUT));
}

// A security descriptor that lets only SYSTEM and administrators touch the pipe. The client (the
// elevated PadDisplay) is an administrator; a normal-rights program cannot connect.
PSECURITY_DESCRIPTOR MakePipeSd() {
    PSECURITY_DESCRIPTOR sd = nullptr;
    // D: owner SYSTEM; allow generic all to SYSTEM (SY) and Administrators (BA). No one else.
    ConvertStringSecurityDescriptorToSecurityDescriptorW(
        L"D:(A;;GA;;;SY)(A;;GA;;;BA)", SDDL_REVISION_1, &sd, nullptr);
    return sd;
}

} // namespace

// ================= main (elevated) side ==========================================================

bool EnsureUacRelayTask(std::wstring& message) {
    DWORD code = 1;
    RunCapture(std::wstring(L"schtasks.exe /query /tn \"") + kUacRelayTask + L"\"", 10000, &code);
    if (code == 0) return true; // already registered

    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    // SYSTEM principal (System integrity), startable on demand by interactive users (GRGX;;;IU), like
    // the virtual-monitor on/off tasks. Its action is fixed: this exe with --uac-relay and nothing else.
    std::wstring xml =
        L"<?xml version=\"1.0\" encoding=\"UTF-16\"?>\r\n"
        L"<Task version=\"1.2\" xmlns=\"http://schemas.microsoft.com/windows/2004/02/mit/task\">\r\n"
        L"  <RegistrationInfo>\r\n"
        L"    <Author>PadDisplay</Author>\r\n"
        L"    <Description>PadDisplay: start the System helper that lets the tablet click UAC prompts (only-screen mode).</Description>\r\n"
        L"    <SecurityDescriptor>D:(A;;FA;;;SY)(A;;FA;;;BA)(A;;GRGX;;;IU)</SecurityDescriptor>\r\n"
        L"  </RegistrationInfo>\r\n"
        L"  <Principals><Principal id=\"System\"><UserId>S-1-5-18</UserId><RunLevel>HighestAvailable</RunLevel></Principal></Principals>\r\n"
        L"  <Settings>\r\n"
        L"    <MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy>\r\n"
        L"    <DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries>\r\n"
        L"    <StopIfGoingOnBatteries>false</StopIfGoingOnBatteries>\r\n"
        L"    <AllowStartOnDemand>true</AllowStartOnDemand>\r\n"
        L"    <Hidden>true</Hidden>\r\n"
        L"    <ExecutionTimeLimit>PT2M</ExecutionTimeLimit>\r\n"
        L"    <Priority>4</Priority>\r\n"
        L"  </Settings>\r\n"
        L"  <Actions Context=\"System\"><Exec>\r\n"
        // The Command element is the bare executable path - Task Scheduler must not see surrounding
        // quotes, or it looks for a file whose name literally contains them and the task fails to start.
        L"    <Command>" + std::wstring(exe) + L"</Command>\r\n"
        L"    <Arguments>--uac-relay</Arguments>\r\n"
        L"  </Exec></Actions>\r\n"
        L"</Task>\r\n";
    const bool ok = CreateTaskFromXml(kUacRelayTask, xml, message);
    LOGI("uac-relay: task %s", ok ? "registered" : "FAILED to register");
    return ok;
}

void RemoveUacRelayTask() {
    DWORD code = 1;
    RunCapture(std::wstring(L"schtasks.exe /delete /f /tn \"") + kUacRelayTask + L"\"", 10000, &code);
}

UacClickClient::UacClickClient(std::function<void(const char*)> notice) : notice_(std::move(notice)) {
    thread_ = std::thread([this] { Worker(); });
}

UacClickClient::~UacClickClient() {
    {
        std::lock_guard lock(mutex_);
        stop_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
    if (pipe_ != INVALID_HANDLE_VALUE) CloseHandle(pipe_);
}

// Enqueue only - called on the input thread, so it must never block. A UAC prompt is clicked at human
// speed, so a tiny bound is plenty; drop the oldest if somehow exceeded rather than grow unbounded.
void UacClickClient::Click(int x, int y) {
    {
        std::lock_guard lock(mutex_);
        if (queue_.size() >= 32) queue_.pop_front();
        queue_.push_back(UacFrame{uint32_t(UacCmd::Click), x, y});
    }
    cv_.notify_one();
}

void UacClickClient::Key(WORD vk) {
    {
        std::lock_guard lock(mutex_);
        if (queue_.size() >= 32) queue_.pop_front();
        queue_.push_back(UacFrame{uint32_t(UacCmd::Key), int32_t(vk), 0});
    }
    cv_.notify_one();
}

void UacClickClient::Worker() {
    for (;;) {
        UacFrame f{};
        {
            std::unique_lock lock(mutex_);
            cv_.wait(lock, [this] { return stop_ || !queue_.empty(); });
            if (stop_ && queue_.empty()) return;
            f = queue_.front();
            queue_.pop_front();
        }
        LOGI("uac-relay: forwarding %s to the injector", f.cmd == uint32_t(UacCmd::Click) ? "a click" : "a key");
        if (!Send(f) && !warned_) {
            warned_ = true;
            if (notice_) notice_("Could not reach the PC helper that clicks UAC prompts. Answer this prompt on the PC. "
                                 "(It is set up when the tablet is the only screen and PadDisplay runs as administrator.)");
        }
    }
}

bool UacClickClient::Ensure() {
    if (pipe_ != INVALID_HANDLE_VALUE) return true;
    if (!triedTask_) {
        triedTask_ = true;
        DWORD code = 1;
        RunCapture(std::wstring(L"schtasks.exe /run /tn \"") + kUacRelayTask + L"\"", 10000, &code);
        if (code != 0) { LOGI("uac-relay: could not start the task (%lu)", code); return false; }
    }
    // The relay spawns the injector, which creates the pipe: wait briefly for it to appear. On the
    // worker thread, so this wait never touches the input path.
    for (int i = 0; i < 30 && pipe_ == INVALID_HANDLE_VALUE; ++i) {
        if (WaitNamedPipeW(kUacPipeName, 100)) {
            HANDLE h = CreateFileW(kUacPipeName, GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
            if (h != INVALID_HANDLE_VALUE) { pipe_ = h; break; }
        }
        Sleep(100);
    }
    if (pipe_ == INVALID_HANDLE_VALUE) LOGI("uac-relay: injector pipe did not open");
    return pipe_ != INVALID_HANDLE_VALUE;
}

bool UacClickClient::Send(const UacFrame& f) {
    // Two tries: the injector is time-limited (the task's ExecutionTimeLimit), so a long session finds
    // it gone and the first write fails; drop the dead pipe and re-launch transparently before giving
    // up. Both the re-trigger and the pipe wait run on this worker thread, never on the input thread.
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (!Ensure()) return false;
        DWORD wrote = 0;
        if (WriteFile(pipe_, &f, sizeof(f), &wrote, nullptr) && wrote == sizeof(f)) {
            warned_ = false; // it is working
            return true;
        }
        CloseHandle(pipe_);
        pipe_ = INVALID_HANDLE_VALUE;
        triedTask_ = false; // next Ensure re-triggers the task
    }
    return false;
}

// ================= SYSTEM relay / injector =======================================================

int RunUacRelayBootstrap() {
    LogInit(AppDataDir() + L"\\uac-relay.log", false, true);
    // We are SYSTEM in session 0. Put a copy of our token into the active console session and start the
    // injector there, so its SendInput reaches the user's desktop (and the consent prompt on it).
    DWORD session = WTSGetActiveConsoleSessionId();
    if (session == 0xFFFFFFFF) { LOGI("uac-relay: no active console session"); return 1; }

    HANDLE self = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(),
                          TOKEN_DUPLICATE | TOKEN_QUERY | TOKEN_ASSIGN_PRIMARY | TOKEN_ADJUST_DEFAULT | TOKEN_ADJUST_SESSIONID,
                          &self)) {
        LOGI("uac-relay: OpenProcessToken failed (%lu)", GetLastError());
        return 1;
    }
    HANDLE dup = nullptr;
    BOOL okDup = DuplicateTokenEx(self, MAXIMUM_ALLOWED, nullptr, SecurityImpersonation, TokenPrimary, &dup);
    CloseHandle(self);
    if (!okDup) { LOGI("uac-relay: DuplicateTokenEx failed (%lu)", GetLastError()); return 1; }
    if (!SetTokenInformation(dup, TokenSessionId, &session, sizeof(session)))
        LOGI("uac-relay: SetTokenInformation(session) failed (%lu)", GetLastError()); // try to start anyway

    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring cmd = std::wstring(L"\"") + exe + L"\" --uac-inject";

    LPVOID env = nullptr;
    CreateEnvironmentBlock(&env, dup, FALSE);
    STARTUPINFOW si{sizeof(si)};
    si.lpDesktop = const_cast<wchar_t*>(L"winsta0\\default");
    PROCESS_INFORMATION pi{};
    const DWORD flags = CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW;
    BOOL ok = CreateProcessAsUserW(dup, exe, cmd.data(), nullptr, nullptr, FALSE, flags, env, nullptr, &si, &pi);
    if (env) DestroyEnvironmentBlock(env);
    CloseHandle(dup);
    if (!ok) { LOGI("uac-relay: CreateProcessAsUser failed (%lu)", GetLastError()); return 1; }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    LOGI("uac-relay: injector started in session %lu", session);
    return 0;
}

int RunUacInjector() {
    LogInit(AppDataDir() + L"\\uac-relay.log", false, true);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2); // match the host's coordinate mapping

    PSECURITY_DESCRIPTOR sd = MakePipeSd();
    SECURITY_ATTRIBUTES sa{sizeof(sa), sd, FALSE};
    // One client at a time (the local PadDisplay). Message mode, 12-byte frames.
    HANDLE pipe = CreateNamedPipeW(kUacPipeName, PIPE_ACCESS_INBOUND,
                                   PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                                   1, 0, sizeof(UacFrame) * 4, 0, sd ? &sa : nullptr);
    if (sd) LocalFree(sd);
    if (pipe == INVALID_HANDLE_VALUE) { LOGI("uac-inject: CreateNamedPipe failed (%lu)", GetLastError()); return 1; }
    LOGI("uac-inject: ready (SYSTEM, this session); waiting for the local client");

    // Serve at most a couple of minutes total, so a forgotten injector does not linger as SYSTEM.
    const ULONGLONG deadline = GetTickCount64() + 2 * 60 * 1000;
    while (GetTickCount64() < deadline) {
        if (!ConnectNamedPipe(pipe, nullptr) && GetLastError() != ERROR_PIPE_CONNECTED) break;
        for (;;) {
            UacFrame f{};
            DWORD got = 0;
            if (!ReadFile(pipe, &f, sizeof(f), &got, nullptr) || got != sizeof(f)) break;
            std::wstring fg = ForegroundImage(); // read once: for the check and the log
            if (_wcsicmp(fg.c_str(), L"consent.exe") != 0) { // authoritative: never inject into anything else
                LOGI("uac-inject: ignored a command - foreground is %ls, not the UAC prompt", fg.c_str());
                continue;
            }
            if (f.cmd == uint32_t(UacCmd::Click)) {
                InjectClick(f.a, f.b);
                LOGI("uac-inject: clicked the UAC prompt at %d,%d", f.a, f.b);
            } else if (f.cmd == uint32_t(UacCmd::Key) && f.a >= 0 && f.a <= 0xFFFF && KeyAllowed(WORD(f.a))) {
                InjectKey(WORD(f.a));
                LOGI("uac-inject: sent key 0x%02x to the UAC prompt", unsigned(f.a));
            }
        }
        DisconnectNamedPipe(pipe);
    }
    CloseHandle(pipe);
    LOGI("uac-inject: exiting");
    return 0;
}

} // namespace pd
