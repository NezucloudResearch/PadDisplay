// PadDisplay by Nezucloud. Copyright (c) 2026 Nezucloud.
// SPDX-License-Identifier: MIT
// The tablet's camera as a PC webcam: one-time setup of the virtual camera (Windows 11's MFCreateVirtualCamera, no
// driver) and its state. The picture itself is carried by camshared.h and drawn by PadDisplayCamera.dll.
#pragma once
#include <string>

namespace pd {

enum class CamState {
    Unsupported,  // before Windows 11 (build 22000): the virtual camera API does not exist
    NotInstalled, // needs the one-time administrator setup
    Ready,
};

// Cheap (registry and file checks only), so it is safe for the tray menu.
CamState QueryCamera();
const wchar_t* CamStateText(CamState s);

// Administrator rights. Copies PadDisplayCamera.dll from packageDir (the "driver\camera" folder next to the exe) to
// Program Files, which only administrators can write: the Frame Server service loads it, so a folder normal users
// can change would be a way to run code as that service. Then registers its COM class and the virtual camera.
bool InstallCamera(const std::wstring& packageDir, std::wstring& message);
bool RemoveCamera(std::wstring& message);

} // namespace pd
