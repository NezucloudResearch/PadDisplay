// PadDisplay by Nezucloud. Copyright (c) 2026 Nezucloud.
// SPDX-License-Identifier: MIT
// The tray icon's menu: what it shows, and the choices that only change a setting.
#pragma once
#include "settings.h"

#include <windows.h>
#include <string>

namespace pd
{

    enum MenuIdDriver : UINT
    {
        ID_DRIVER = 50,
        ID_GET_VIGEM,
        ID_ENABLE_VIGEM,
        ID_SETUP_CAMERA,
        ID_ACCEPT_MIC,
        ID_ACCEPT_CAM,
        ID_SETUP_MIC,
        ID_REMOVE_MIC,
        ID_SETUP_CABLE,
        ID_ABOUT_CABLE
    };

    enum MenuId : UINT
    {
        ID_EXIT = 100,
        ID_OPEN_LOG,
        ID_OPEN_FOLDER,
        ID_ADAPTIVE,
        ID_CURSOR,
        ID_MATCH_RES,
        ID_DETACH,
        ID_AUTOSTART,
        ID_ADMIN,
        ID_UAC_CLICK,
        ID_LOCK_SCREEN,
        ID_PRESIGNIN,
        ID_CODEC_AUTO = 200,
        ID_CODEC_HEVC,
        ID_CODEC_H264,
        ID_USB_RATE = 300,  // + Mbps
        ID_WIFI_RATE = 400, // + Mbps
        ID_GPU_AUTO = 500,
        ID_GPU_FIRST, // ID_GPU_FIRST + index into ListEncodeGpus()
    };

    // What the menu shows that the settings do not say.
    struct TrayMenuState
    {
        std::wstring status;
        std::string tlsFingerprint;
        bool installing = false;
        int setupIncomplete = -1;
        bool micNeedsRepair = false;
        bool autostart = false;
    };

    void ShowTrayMenu(HWND owner, const Settings &settings, const TrayMenuState &state);

    bool IsGpuChoice(UINT id);
    void ApplyMenuChoice(UINT id, Settings &settings);

} // namespace pd
