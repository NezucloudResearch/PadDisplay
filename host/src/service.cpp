// PadDisplay by Nezucloud. Copyright (c) 2026 Nezucloud.
// SPDX-License-Identifier: MIT
#include "service.h"
#include "display.h"  // SnapshotRealDisplays
#include "log.h"
#include "net.h"      // NetStartup
#include "session.h"  // Server
#include "settings.h"
#include "tls.h"      // TlsIdentity (machine-wide identity)

#include <windows.h>
#include <winsvc.h>
#include <wtsapi32.h>
#include <userenv.h>
#include <sddl.h>
#include <aclapi.h>
#include <atomic>
#include <string>

namespace pd {
namespace {

// A machine-readable copy of the few settings the service needs, since it runs as LocalSystem and cannot
// read the user's %LOCALAPPDATA%\PadDisplay\settings.ini. The tray host writes it (admin-only DACL) when
// pre-sign-in access is on; the logon-host reads it. It holds the PIN, so it is locked to SYSTEM + admins.
std::wstring MachineConfigPath() {
    wchar_t pd[MAX_PATH] = {};
    DWORD n = GetEnvironmentVariableW(L"ProgramData", pd, MAX_PATH);
    std::wstring dir = (n > 0 && n < MAX_PATH) ? std::wstring(pd) : std::wstring(L"C:\\ProgramData");
    dir += L"\\PadDisplay";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir + L"\\service.ini";
}

void RestrictToAdmins(const std::wstring& path) {
    // SYSTEM and administrators full control, nobody else; protected (no inherited ACEs).
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;;FA;;;SY)(A;;FA;;;BA)", SDDL_REVISION_1, &sd, nullptr)) {
        BOOL present = FALSE, defaulted = FALSE;
        PACL dacl = nullptr;
        if (GetSecurityDescriptorDacl(sd, &present, &dacl, &defaulted) && present)
            SetNamedSecurityInfoW(const_cast<wchar_t*>(path.c_str()), SE_FILE_OBJECT,
                                  DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, nullptr, nullptr, dacl, nullptr);
        LocalFree(sd);
    }
}

constexpr wchar_t kLogonHostStopEvent[] = L"Global\\PadDisplayLogonHostStop";

// The GDI name of the primary display (the one at the desktop origin), to capture the real sign-in screen.
std::wstring PrimaryDisplayName() {
    for (const auto& d : SnapshotRealDisplays())
        if (d.x == 0 && d.y == 0) return d.gdiName;
    auto all = SnapshotRealDisplays();
    return all.empty() ? std::wstring() : all.front().gdiName;
}

// Duplicate this (SYSTEM) process's token into the active console session and start [cmdline] there on
// the Winlogon desktop, so it can capture and inject the sign-in screen. Same technique as the UAC relay.
HANDLE StartInConsoleSession(const std::wstring& cmdline) {
    DWORD session = WTSGetActiveConsoleSessionId();
    if (session == 0xFFFFFFFF) { LOGI("service: no active console session yet; will retry"); return nullptr; }
    HANDLE self = nullptr, dup = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(),
                          TOKEN_DUPLICATE | TOKEN_QUERY | TOKEN_ASSIGN_PRIMARY | TOKEN_ADJUST_DEFAULT | TOKEN_ADJUST_SESSIONID, &self))
        return nullptr;
    BOOL ok = DuplicateTokenEx(self, MAXIMUM_ALLOWED, nullptr, SecurityImpersonation, TokenPrimary, &dup);
    CloseHandle(self);
    if (!ok) return nullptr;
    SetTokenInformation(dup, TokenSessionId, &session, sizeof(session));

    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring cmd = std::wstring(L"\"") + exe + L"\" " + cmdline;
    LPVOID env = nullptr;
    CreateEnvironmentBlock(&env, dup, FALSE);
    STARTUPINFOW si{sizeof(si)};
    si.lpDesktop = const_cast<wchar_t*>(L"winsta0\\winlogon");
    PROCESS_INFORMATION pi{};
    ok = CreateProcessAsUserW(dup, exe, cmd.data(), nullptr, nullptr, FALSE,
                              CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW, env, nullptr, &si, &pi);
    if (env) DestroyEnvironmentBlock(env);
    CloseHandle(dup);
    if (!ok) { LOGI("service: could not start the logon-host in session %lu (%lu)", session, GetLastError()); return nullptr; }
    CloseHandle(pi.hThread);
    LOGI("service: logon-host started in session %lu", session);
    return pi.hProcess;
}

// Is a real user signed in on the console session? (Signed in and then locked still counts: the tray host
// serves the locked screen, so the service stays off.)
//
// The reliable signal is whether the console session holds an interactive user token, not the WTSUserName
// string. With Fast Startup (HiberbootEnabled, the default on this and most Windows 11 PCs) a shutdown is a
// partial hibernate, and on the next boot a stale user name lingers on the console session at the sign-in
// screen. Trusting the name made the service believe someone was already signed in and never launch the
// logon-host - exactly the "nothing works before login" fault. A user token exists only once a real sign-in
// has completed, so WTSQueryUserToken is the honest test; SYSTEM has the SeTcbPrivilege it needs.
bool UserSignedInOnConsole() {
    DWORD session = WTSGetActiveConsoleSessionId();
    if (session == 0xFFFFFFFF) return false; // no console session yet (early boot): serve
    HANDLE token = nullptr;
    const bool signedIn = WTSQueryUserToken(session, &token) != 0;
    if (token) CloseHandle(token);

    // Log only when the picture changes, so a shipping service stays quiet but every transition is captured.
    static DWORD lastSession = 0xFFFFFFFE;
    static int lastSignedIn = -1;
    if (session != lastSession || int(signedIn) != lastSignedIn) {
        LPWSTR user = nullptr;
        DWORD n = 0;
        std::wstring name;
        if (WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, session, WTSUserName, &user, &n)) {
            if (user) name = user;
            WTSFreeMemory(user);
        }
        LOGI("service: console session %lu name='%ls' userToken=%d -> %s", session, name.c_str(),
             signedIn ? 1 : 0, signedIn ? "signed in (stay off)" : "sign-in screen (serve)");
        lastSession = session;
        lastSignedIn = int(signedIn);
    }
    return signedIn;
}

SERVICE_STATUS_HANDLE g_ssh = nullptr;
SERVICE_STATUS g_ss{};
HANDLE g_stopEvent = nullptr;   // the service is stopping
HANDLE g_reevalEvent = nullptr; // a session change: re-decide whether to serve
HANDLE g_hostProc = nullptr;    // the running logon-host, or null
DWORD g_hostSession = 0xFFFFFFFF; // the console session it was launched in

void Report(DWORD state, DWORD wait = 0) {
    g_ss.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    g_ss.dwCurrentState = state;
    g_ss.dwControlsAccepted = state == SERVICE_RUNNING ? (SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN | SERVICE_ACCEPT_SESSIONCHANGE) : 0;
    g_ss.dwWaitHint = wait;
    g_ss.dwCheckPoint = (state == SERVICE_RUNNING || state == SERVICE_STOPPED) ? 0 : g_ss.dwCheckPoint + 1;
    if (g_ssh) SetServiceStatus(g_ssh, &g_ss);
}

void StopLogonHost() {
    if (!g_hostProc) return;
    if (HANDLE ev = OpenEventW(EVENT_MODIFY_STATE, FALSE, kLogonHostStopEvent)) { SetEvent(ev); CloseHandle(ev); }
    if (WaitForSingleObject(g_hostProc, 4000) != WAIT_OBJECT_0) TerminateProcess(g_hostProc, 0);
    CloseHandle(g_hostProc);
    g_hostProc = nullptr;
    LOGI("service: logon-host stopped");
}

// Launch or stop the logon-host so it runs exactly while no one is signed in, in exactly one console
// session. During a sign-out the active console session changes (the dying session, then the fresh
// sign-in screen); relaunching in the new one - after stopping the old so they never both bind 27183 -
// keeps it converged on the session actually showing the sign-in screen.
void Reevaluate() {
    if (g_hostProc && WaitForSingleObject(g_hostProc, 0) == WAIT_OBJECT_0) { CloseHandle(g_hostProc); g_hostProc = nullptr; }
    const bool serve = !UserSignedInOnConsole();
    const DWORD current = WTSGetActiveConsoleSessionId();
    if (serve) {
        if (g_hostProc && g_hostSession != current) StopLogonHost(); // console session moved: relaunch in it
        if (!g_hostProc) { g_hostProc = StartInConsoleSession(L"--logon-host"); g_hostSession = current; }
    } else if (g_hostProc) {
        StopLogonHost();
    }
}

DWORD WINAPI ServiceCtrl(DWORD ctrl, DWORD, LPVOID, LPVOID) {
    switch (ctrl) {
    case SERVICE_CONTROL_STOP:
    case SERVICE_CONTROL_SHUTDOWN:
        Report(SERVICE_STOP_PENDING, 5000);
        SetEvent(g_stopEvent);
        return NO_ERROR;
    case SERVICE_CONTROL_SESSIONCHANGE:
        SetEvent(g_reevalEvent);
        return NO_ERROR;
    case SERVICE_CONTROL_INTERROGATE:
        return NO_ERROR;
    default:
        return ERROR_CALL_NOT_IMPLEMENTED;
    }
}

void WINAPI ServiceMain(DWORD, LPWSTR*) {
    g_ssh = RegisterServiceCtrlHandlerExW(kServiceName, ServiceCtrl, nullptr);
    if (!g_ssh) return;
    Report(SERVICE_START_PENDING, 3000);
    g_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_reevalEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    Report(SERVICE_RUNNING);
    LOGI("service: running");

    Reevaluate();
    HANDLE waits[2] = {g_stopEvent, g_reevalEvent};
    for (;;) {
        // Also wake every few seconds: some session transitions at the sign-in screen do not raise a
        // session-change control, and the logon-host may have exited on its own (a finished connection).
        DWORD w = WaitForMultipleObjects(2, waits, FALSE, 3000);
        if (w == WAIT_OBJECT_0) break; // stop
        Reevaluate();
    }
    StopLogonHost();
    Report(SERVICE_STOPPED);
    LOGI("service: stopped");
}

SC_HANDLE OpenScm(DWORD access) { return OpenSCManagerW(nullptr, nullptr, access); }

} // namespace

void SyncMachineConfig(const Settings& s) {
    const std::wstring path = MachineConfigPath();
    auto put = [&](const wchar_t* k, long long v) { WritePrivateProfileStringW(L"host", k, std::to_wstring(v).c_str(), path.c_str()); };
    auto puts = [&](const wchar_t* k, const std::wstring& v) { WritePrivateProfileStringW(L"host", k, v.c_str(), path.c_str()); };
    put(L"pin", s.pin);
    put(L"usbBitrateMbps", s.usbBitrateMbps);
    put(L"wifiBitrateMbps", s.wifiBitrateMbps);
    put(L"minBitrateMbps", s.minBitrateMbps);
    put(L"adaptive", s.adaptive);
    put(L"maxFramesInFlight", s.maxFramesInFlight);
    puts(L"codec", s.codec);
    put(L"maxFps", s.maxFps);
    puts(L"encoder", s.encoder);
    puts(L"gpu", s.gpu);
    put(L"nvencPreset", s.nvencPreset);
    put(L"drawCursor", s.drawCursor);
    put(L"preSignInAccess", s.preSignInAccess);
    RestrictToAdmins(path); // the PIN is in here: SYSTEM + administrators only
}

static void LoadMachineConfig(Settings& s) {
    const std::wstring path = MachineConfigPath();
    auto geti = [&](const wchar_t* k, int def) { return int(GetPrivateProfileIntW(L"host", k, def, path.c_str())); };
    auto gets = [&](const wchar_t* k, const std::wstring& def) { wchar_t b[256]; GetPrivateProfileStringW(L"host", k, def.c_str(), b, 256, path.c_str()); return std::wstring(b); };
    s.pin = uint32_t(GetPrivateProfileIntW(L"host", L"pin", 0, path.c_str()));
    s.usbBitrateMbps = geti(L"usbBitrateMbps", s.usbBitrateMbps);
    s.wifiBitrateMbps = geti(L"wifiBitrateMbps", s.wifiBitrateMbps);
    s.minBitrateMbps = geti(L"minBitrateMbps", s.minBitrateMbps);
    s.adaptive = geti(L"adaptive", s.adaptive) != 0;
    s.maxFramesInFlight = geti(L"maxFramesInFlight", s.maxFramesInFlight);
    s.codec = gets(L"codec", s.codec);
    s.maxFps = geti(L"maxFps", s.maxFps);
    s.encoder = gets(L"encoder", s.encoder);
    s.gpu = gets(L"gpu", s.gpu);
    s.nvencPreset = geti(L"nvencPreset", s.nvencPreset);
    s.drawCursor = geti(L"drawCursor", s.drawCursor) != 0;
    s.preSignInAccess = geti(L"preSignInAccess", s.preSignInAccess) != 0;
}

bool LogonServiceInstalled() {
    SC_HANDLE scm = OpenScm(SC_MANAGER_CONNECT);
    if (!scm) return false;
    SC_HANDLE svc = OpenServiceW(scm, kServiceName, SERVICE_QUERY_STATUS);
    bool there = svc != nullptr;
    if (svc) CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return there;
}

bool InstallLogonService(std::wstring& message) {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring bin = std::wstring(L"\"") + exe + L"\" --service";
    SC_HANDLE scm = OpenScm(SC_MANAGER_CREATE_SERVICE);
    if (!scm) { message = L"Could not open the service manager (administrator rights needed)."; return false; }
    SC_HANDLE svc = CreateServiceW(scm, kServiceName, L"PadDisplay (sign-in screen access)", SERVICE_ALL_ACCESS,
                                   SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START, SERVICE_ERROR_NORMAL,
                                   bin.c_str(), nullptr, nullptr, nullptr, nullptr, nullptr);
    if (!svc) {
        DWORD e = GetLastError();
        if (e == ERROR_SERVICE_EXISTS) {
            svc = OpenServiceW(scm, kServiceName, SERVICE_ALL_ACCESS | SERVICE_CHANGE_CONFIG);
            if (svc) ChangeServiceConfigW(svc, SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START, SERVICE_ERROR_NORMAL,
                                          bin.c_str(), nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
        }
        if (!svc) { message = L"Could not create the service (error " + std::to_wstring(e) + L")."; CloseServiceHandle(scm); return false; }
    }
    SERVICE_DESCRIPTIONW d{const_cast<wchar_t*>(L"Lets a paired PadDisplay tablet reach the Windows sign-in screen before anyone signs in.")};
    ChangeServiceConfig2W(svc, SERVICE_CONFIG_DESCRIPTION, &d);
    // Make the machine-wide TLS identity now, while we are elevated, so the service and the tray host share
    // one fingerprint from the first boot instead of racing to create it (and the tablet re-trusts only once).
    if (auto id = TlsIdentity::LoadOrCreate(true)) LOGI("service: machine TLS identity ready, fingerprint %s", id->Fingerprint().c_str());
    else LOGI("service: could not create the machine TLS identity");
    StartServiceW(svc, 0, nullptr); // ok if it is already running
    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    LOGI("service: installed");
    return true;
}

bool RemoveLogonService(std::wstring& message) {
    SC_HANDLE scm = OpenScm(SC_MANAGER_CONNECT);
    if (!scm) { message = L"Could not open the service manager."; return false; }
    SC_HANDLE svc = OpenServiceW(scm, kServiceName, SERVICE_STOP | DELETE);
    if (!svc) { CloseServiceHandle(scm); return true; } // not installed: nothing to do
    SERVICE_STATUS st{};
    ControlService(svc, SERVICE_CONTROL_STOP, &st);
    bool ok = DeleteService(svc) != 0;
    if (!ok) message = L"Could not remove the service (error " + std::to_wstring(GetLastError()) + L").";
    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    DeleteFileW(MachineConfigPath().c_str());
    if (ok) LOGI("service: removed");
    return ok;
}

int RunLogonService() {
    LogInit(std::wstring(L"C:\\ProgramData\\PadDisplay\\service.log"), false, true);
    SERVICE_TABLE_ENTRYW table[] = {{const_cast<wchar_t*>(kServiceName), ServiceMain}, {nullptr, nullptr}};
    if (!StartServiceCtrlDispatcherW(table)) { LOGI("service: dispatcher failed (%lu)", GetLastError()); return 1; }
    return 0;
}

int RunLogonHost() {
    LogInit(std::wstring(L"C:\\ProgramData\\PadDisplay\\logon-host.log"), false, true);
    Settings s;
    LoadMachineConfig(s);
    if (s.pin < 100000 || s.pin > 999999) { LOGI("logon-host: no valid PIN in the machine config; not serving"); return 1; }
    s.displayOverride = PrimaryDisplayName(); // capture the real sign-in screen, not a virtual monitor
    if (s.displayOverride.empty()) { LOGI("logon-host: no primary display found; not serving"); return 1; }
    s.requireEncryption = true;  // this carries the Windows password: Wi-Fi must be encrypted
    s.preSignInAccess = true;    // force the machine-wide TLS identity: a SYSTEM per-user key is useless here
    s.lockScreenControl = true;  // the logon host exists to type the sign-in password; without this the session
                                 // would offer the tablet's password box but drop every character it sent. The
                                 // machine config does not carry this flag, so set it here (the user opted into
                                 // pre-sign-in access by installing the service; see SECURITY.md).
    s.monitorOnlyWhileConnected = false;
    s.removeMonitorOnExit = false;
    LOGI("logon-host: serving the sign-in screen %ls (Wi-Fi + TLS only)", s.displayOverride.c_str());

    if (!NetStartup()) { LOGI("logon-host: WSAStartup failed"); return 1; }
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    HANDLE stop = CreateEventW(nullptr, TRUE, FALSE, kLogonHostStopEvent);

    Server server(s, [](const std::wstring&) {}); // no tray UI here
    if (!server.Start()) { LOGI("logon-host: could not start the server"); if (stop) CloseHandle(stop); return 1; }
    WaitForSingleObject(stop, INFINITE); // the service signals this to stop us
    server.Stop();
    if (stop) CloseHandle(stop);
    CoUninitialize();
    LOGI("logon-host: stopped");
    return 0;
}

} // namespace pd
