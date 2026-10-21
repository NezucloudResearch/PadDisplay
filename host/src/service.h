// PadDisplay by Nezucloud. Copyright (c) 2026 Nezucloud.
// SPDX-License-Identifier: MIT
//
// Boot / before-sign-in access (piece 4). A Windows service (LocalSystem, auto-start) serves the tablet
// on TCP 27183 ONLY while no one is signed in - at boot, and after a sign-out. When a user signs in, the
// normal tray host (started by the sign-in task) takes the port, and the service steps back; the locked
// case after sign-in is handled by the tray host itself (lockcapture.* / uacrelay.*). So the service and
// the tray host never fight for the port.
//
// While no one is signed in, the service launches "PadDisplay.exe --logon-host" as SYSTEM on the console
// session's Winlogon desktop. That process captures the real primary screen's sign-in UI and injects the
// tablet's input directly (it is SYSTEM on that desktop, so capture and injection are permitted). It is
// Wi-Fi + TLS only (adb/USB is not up before sign-in) and reads its PIN and stream settings from a
// machine-readable config the tray host writes. Everything here is OFF unless the user installs it
// (--install-service, gated by the preSignInAccess setting). Security-relevant: see SECURITY.md.
#pragma once
#include <string>

namespace pd {

struct Settings;

constexpr wchar_t kServiceName[] = L"PadDisplayLogon";

// Mirror the few settings the service needs (PIN + stream settings) to a machine-readable, admin-only
// file, because the service runs as LocalSystem and cannot read the user's settings.ini. Called by the
// tray host when pre-sign-in access is on.
void SyncMachineConfig(const Settings& s);

// --install-service / --remove-service (administrator). Create/remove the auto-start service.
bool InstallLogonService(std::wstring& message);
bool RemoveLogonService(std::wstring& message);
bool LogonServiceInstalled();

// Entry point for the service process (dispatched from wWinMain on --service). Blocks until stopped.
int RunLogonService();

// Entry point for the per-session worker the service launches (--logon-host). Blocks until the service
// signals it to stop (named stop event) or the connection ends. Runs as SYSTEM on the Winlogon desktop.
int RunLogonHost();

} // namespace pd
