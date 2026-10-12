// PadDisplay by Nezucloud. Copyright (c) 2026 Nezucloud.
// SPDX-License-Identifier: MIT
// Virtual Display Driver (IddCx, MikeTheTech/VirtualDrivers) management: detection, one-time
// install from the bundled driver package, and adding display modes at runtime.
#pragma once
#include <string>
#include <vector>

namespace pd {

enum class VddState { NotInstalled, Disabled, Problem, Ready };

VddState QueryVdd(std::wstring* detail = nullptr);
const wchar_t* VddStateText(VddState s);

// Needs administrator rights. Writes a default vdd_settings.xml if there is none, enables the
// existing device or creates the root device and installs driverDir\MttVDD.inf, and registers the
// on/off scheduled tasks (see SetVirtualMonitorEnabled).
bool InstallVdd(const std::wstring& driverDir, std::wstring& message);
// Needs administrator rights (the uninstaller). Removes the device, its driver package and the settings file PadDisplay
// wrote for it.
bool RemoveVdd(std::wstring& message);
// Needs administrator rights (the uninstaller). Deletes every on/off task PadDisplay registered, whoever installed the drivers.
void RemoveToggleTasks();

// The driver always creates at least one monitor, so "no virtual monitor" means disabling the
// device, which needs admin. InstallVdd registers two hidden scheduled tasks that run pnputil as
// SYSTEM; normal users may start them, so PadDisplay can switch the monitor off on exit and back on
// at start without a UAC prompt. Returns false if the tasks are missing or the state did not change.
bool ToggleTasksExist();
bool SetVirtualMonitorEnabled(bool on, unsigned timeoutMs);

// Set when a tablet was told to fix the virtual monitor from the tray. The tray then offers that fix even where it
// normally hides it (the monitor is off between sessions on purpose). Cleared once a session gets its monitor.
void SetVddFailed(bool failed);
bool VddFailed();

// Escapes &, < and > for text inside task XML (a path such as C:\R&D\PadDisplay.exe).
std::wstring XmlEscape(const std::wstring& s);

// Registers (or replaces) a scheduled task from its XML. Needs administrator rights.
bool CreateTaskFromXml(const wchar_t* name, const std::wstring& xml, std::wstring& message);

// Makes sure width x height and hz are listed in vdd_settings.xml; if the file had to change, asks
// the running driver to reload it (no administrator rights needed). changed reports a reload.
bool EnsureVddMode(int width, int height, int hz, bool& changed);

std::wstring VddSettingsPath();

// ViGEmBus (Nefarius), the virtual game controller bus. The installer ships in driver\vigem (BSD-3-Clause).
VddState QueryVigem();
// Needs administrator rights. Runs the bundled installer silently if ViGEmBus is not installed. The installer is checked
// against a pinned SHA-256 and its Authenticode signature first: it runs with administrator rights, so nothing that is
// not exactly the shipped file may run.
bool InstallVigemBus(const std::wstring& driverDir, std::wstring& message);
// The check InstallVigemBus makes before running the installer, on its own (for tests).
bool VerifyVigemInstaller(const std::wstring& path, std::wstring& message);
// Needs administrator rights (part of the one-time setup run). Switches the bus device back on if
// it is disabled or stuck, and registers a hidden task that lets normal users switch it on.
bool SetupVigem(std::wstring& message);
// Starts that task. false = it is not registered (no admin setup run since ViGEmBus was installed).
bool EnableVigem();
// Needs administrator rights (the uninstaller). ViGEmBus is a Windows Installer product: asks Windows Installer to remove it,
// without a window.
bool RemoveVigem(std::wstring& message);

// The PadDisplay Microphone driver (acxmic, at host\third_party\padmic; KMDF + ACX), the virtual microphone the tablet's microphone plays
// into. Unlike the virtual monitor it is installed on request only (tray menu), not by the general setup: it adds a
// recording device to Windows and a kernel driver, which only people who use the tablet's microphone want.
VddState QueryMic(std::wstring* detail = nullptr);
// Needs administrator rights. Creates the root device and installs driverDir\padmic.inf, or repairs an existing device.
bool InstallMicDriver(const std::wstring& driverDir, std::wstring& message);
// Loads or unloads the driver by enabling or disabling its device: no administrator rights (the install registers two tasks that
// run pnputil as SYSTEM, like the virtual monitor's). Blocks up to timeoutMs: never call it from a hot thread or the UI thread.
// false = not installed, no tasks (run the repair), or it did not change in time.
bool SetMicDriverEnabled(bool on, unsigned timeoutMs);
// How many apps are recording from the PadDisplay microphone right now (asked of the driver through its control device, which is
// exclusive: PadDisplay's own microphone session must be closed). 0 = none, or the driver is not loaded. -1 = cannot tell (the
// device is held by something else). Disabling or removing the device while an app records makes Windows' audio engine refuse, and
// Windows then keeps the device "pending restart": it cannot be switched on or off again until a restart, and comes back broken.
int MicRecorders();
// The microphone device is waiting for a restart to finish a change (a refused disable, see above).
bool MicRestartPending();
// Needs administrator rights. Removes the device, its driver package and the two tasks.
bool RemoveMicDriver(std::wstring& message);

// Signature helpers (WinVerifyTrust; no network). A file Windows trusts at all / a catalog signed by Microsoft's hardware
// publisher, which is what a kernel driver needs to load on a PC with Secure Boot (attestation or WHQL signing).
bool HasValidSignature(const std::wstring& path);
bool SignedByMicrosoftHardware(const std::wstring& catalogPath);
// The certificate that signed a file or a catalog (DER bytes) and the name on it. It says who signed, not whether the
// signature is good: ask HasValidSignature for that.
bool SignerCertificate(const std::wstring& signedPath, std::vector<unsigned char>& der, std::wstring& name);
// SHA-256 of a file as lower-case hex: how a shipped file is told from a replaced one before it is trusted or run.
bool Sha256Hex(const std::wstring& path, std::string& hex);
// Is test-signing on (bcdedit /set testsigning on)? Only then does Windows load a driver that Microsoft did not sign.
bool TestSigningOn();

// VB-CABLE (VB-Audio, donationware, shipped unmodified in driver\vbcable): the fallback virtual microphone on PCs where
// PadDisplay's own driver cannot load. Needs administrator rights. Checks the files against the pinned hashes and their
// signatures, runs VB-Audio's installer without its window, and waits for the "CABLE Input" device to appear.
bool InstallCable(const std::wstring& driverDir, std::wstring& message);
// Needs administrator rights (the uninstaller). Runs VB-Audio's own uninstall (the same checked installer, without its window).
bool RemoveCable(const std::wstring& driverDir, std::wstring& message);

} // namespace pd
