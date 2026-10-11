// PadDisplay by Nezucloud. Copyright (c) 2026 Nezucloud.
// SPDX-License-Identifier: MIT
// What the installer and the uninstaller need beyond the drivers themselves: a record of what PadDisplay added to this PC,
// whether the exe sits where only administrators can replace it, and the publisher of the virtual monitor driver.
#pragma once
#include <string>

namespace pd {

// A component PadDisplay installed itself (not one that was on the PC before): the uninstaller removes exactly these.
// Kept as values under HKLM\SOFTWARE\PadDisplay, so marking needs administrator rights.
inline constexpr wchar_t kAddedMonitor[] = L"AddedVirtualMonitor";
inline constexpr wchar_t kAddedVigem[] = L"AddedViGEmBus";
inline constexpr wchar_t kAddedCable[] = L"AddedVBCable";
void MarkAddedByPadDisplay(const wchar_t* component, bool added);
bool AddedByPadDisplay(const wchar_t* component);
// Needs administrator rights. Removes HKLM\SOFTWARE\PadDisplay once nothing is recorded in it any more.
void ForgetSetupRecords();

// Can only administrators (and Windows itself) change this file, its folder and the folders above it? A program that a
// scheduled task starts with administrator rights must be: otherwise anything running as the normal user could replace it,
// or put a DLL next to it, and be an administrator at the next sign-in. True for Program Files, false for a folder in the
// user's profile or a folder on another drive that every user may write to.
bool OnlyAdminsCanChange(const std::wstring& path);

// Windows asks "Would you like to install this device software?" for a driver whose publisher is not in the PC's Trusted
// Publishers store. Of the bundled drivers only the virtual monitor's is signed by its publisher (SignPath Foundation) rather
// than by Microsoft, so only that one asks. Needs administrator rights. Adds the certificate that signed the shipped catalog
// (checked against a pinned SHA-256 first) to that store, and remembers that PadDisplay did so.
// The cost: Windows then installs any driver signed by that publisher without asking.
bool TrustDriverPublishers(const std::wstring& driverDir, std::wstring& message);
// Needs administrator rights. Takes out again what TrustDriverPublishers added (and nothing that was there before).
bool UntrustDriverPublishers(std::wstring& message);

// Needs administrator rights. Deletes the (empty) "\PadDisplay" folder in the Task Scheduler.
void DeleteTaskFolder();

// The full path of a program in System32, so an administrator process never runs a same-named file from somewhere else.
std::wstring SystemProgram(const wchar_t* name);

} // namespace pd
