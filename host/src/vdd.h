// PadDisplay by Nezucloud. Copyright (c) 2026 Nezucloud.
// SPDX-License-Identifier: MIT
// Virtual Display Driver (IddCx, MikeTheTech/VirtualDrivers) management: detection, one-time
// install from the bundled driver package, and adding display modes at runtime.
#pragma once
#include <string>

namespace pd {

enum class VddState { NotInstalled, Disabled, Problem, Ready };

VddState QueryVdd(std::wstring* detail = nullptr);
const wchar_t* VddStateText(VddState s);

// Needs administrator rights. Writes a default vdd_settings.xml if there is none, enables the
// existing device or creates the root device and installs driverDir\MttVDD.inf, and registers the
// on/off scheduled tasks (see SetVirtualMonitorEnabled).
bool InstallVdd(const std::wstring& driverDir, std::wstring& message);

// The driver always creates at least one monitor, so "no virtual monitor" means disabling the
// device, which needs admin. InstallVdd registers two hidden scheduled tasks that run pnputil as
// SYSTEM; normal users may start them, so PadDisplay can switch the monitor off on exit and back on
// at start without a UAC prompt. Returns false if the tasks are missing or the state did not change.
bool ToggleTasksExist();
bool SetVirtualMonitorEnabled(bool on, unsigned timeoutMs);

// Registers (or replaces) a scheduled task from its XML. Needs administrator rights.
bool CreateTaskFromXml(const wchar_t* name, const std::wstring& xml, std::wstring& message);

// Makes sure width x height and hz are listed in vdd_settings.xml; if the file had to change, asks
// the running driver to reload it (no administrator rights needed). changed reports a reload.
bool EnsureVddMode(int width, int height, int hz, bool& changed);

std::wstring VddSettingsPath();

// ViGEmBus (Nefarius), the virtual game controller bus. It is not bundled: the user installs it.
VddState QueryVigem();
// Needs administrator rights (part of the one-time setup run). Switches the bus device back on if
// it is disabled or stuck, and registers a hidden task that lets normal users switch it on.
bool SetupVigem(std::wstring& message);
// Starts that task. false = it is not registered (no admin setup run since ViGEmBus was installed).
bool EnableVigem();

} // namespace pd
