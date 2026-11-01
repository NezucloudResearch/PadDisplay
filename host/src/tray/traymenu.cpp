// PadDisplay by Nezucloud. Copyright (c) 2026 Nezucloud.
// SPDX-License-Identifier: MIT
#include "traymenu.h"

#include "camera/vcam.h"
#include "display/securedesktop.h"
#include "encoder/gpu_select.h"
#include "mic/micbackend.h"
#include "secure/service.h"
#include "setup/vdd.h"
#include "text.h"

#include <algorithm>
#include <initializer_list>
#include <vector>

#define PD_WIDEN2(x) L##x
#define PD_WIDEN(x) PD_WIDEN2(x)
#define PD_VERSION_W PD_WIDEN(PD_VERSION_STR)

namespace pd
{
    namespace
    {

        constexpr UINT kMaxGpuChoices = 16;

        UINT CheckedIf(bool on) { return on ? MF_CHECKED : 0; }

        void AddGrayed(HMENU menu, const wchar_t *text) { AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, text); }

        void AddItem(HMENU menu, UINT id, const wchar_t *text) { AppendMenuW(menu, MF_STRING, id, text); }

        void AddCheck(HMENU menu, UINT id, bool on, const wchar_t *text) { AppendMenuW(menu, MF_STRING | CheckedIf(on), id, text); }

        void AddSeparator(HMENU menu) { AppendMenuW(menu, MF_SEPARATOR, 0, nullptr); }

        void AddSubmenu(HMENU menu, HMENU sub, const wchar_t *text) { AppendMenuW(menu, MF_POPUP, UINT_PTR(sub), text); }

        void AddStatusLines(HMENU menu, const Settings &settings, const TrayMenuState &state)
        {
            AddGrayed(menu, L"PadDisplay " PD_VERSION_W L" by Nezucloud");
            AddGrayed(menu, state.status.c_str());
            AddGrayed(menu, (L"PIN: " + std::to_wstring(settings.pin)).c_str());
            // The tablet remembers this the first time it connects over Wi-Fi and refuses the PC if it ever changes.
            AddGrayed(menu, state.tlsFingerprint.empty() ? L"Wi-Fi encryption unavailable (see log)"
                                                         : (L"Wi-Fi encrypted (TLS), fingerprint " + Widen(state.tlsFingerprint.substr(0, 23)) + L"…").c_str());
        }

        void AddMonitorDriverItem(HMENU menu, const Settings &settings, const TrayMenuState &state)
        {
            const VddState vdd = QueryVdd();
            // A tablet was told to choose one of the items below: it must be there, whatever would normally hide it.
            const bool vddFailed = VddFailed();
            if (state.installing)
            {
                AddGrayed(menu, L"Installing virtual monitor driver…");
            }
            else if (vdd == VddState::Ready && state.setupIncomplete == 1 && !vddFailed)
            {
                AddItem(menu, ID_DRIVER, L"Finish PadDisplay setup (admin)… (monitor off on exit, full GPU speed)");
            }
            else if (vdd == VddState::Disabled && settings.monitorOnlyWhileConnected && state.setupIncomplete == 0 && !vddFailed)
            {
                // Off because no tablet is connected: PadDisplay switches it on when one does. Nothing to fix.
                AddGrayed(menu, L"Virtual monitor: off until a tablet connects");
            }
            else if (vdd != VddState::Ready || vddFailed)
            {
                AppendMenuW(menu, MF_STRING | MF_DEFAULT, ID_DRIVER,
                            vdd == VddState::NotInstalled ? L"Install virtual monitor driver (admin)…"
                            : vdd == VddState::Disabled   ? L"Enable virtual monitor driver (admin)…"
                                                          : L"Repair virtual monitor driver (admin)…");
            }
        }

        // Game controllers connected to the tablet need ViGEmBus, which ships with PadDisplay and is installed by the admin setup.
        void AddGamepadItem(HMENU menu, bool installing)
        {
            if (installing)
                return;
            const VddState vigem = QueryVigem();
            if (vigem == VddState::NotInstalled)
                AddItem(menu, ID_GET_VIGEM, L"Install game controller driver (admin)…");
            else if (vigem != VddState::Ready)
                AddItem(menu, ID_ENABLE_VIGEM, L"Enable game controller driver (admin)…");
        }

        // The tablet's camera as a PC webcam (Windows 11).
        void AddCameraItem(HMENU menu, const Settings &settings, bool installing)
        {
            const CamState cam = QueryCamera();
            if (cam == CamState::NotInstalled && !installing)
                AddItem(menu, ID_SETUP_CAMERA, L"Set up the tablet's camera as a webcam (admin)…");
            else if (cam == CamState::Unsupported)
                AddGrayed(menu, L"Tablet camera as webcam: needs Windows 11");
            else if (cam == CamState::Ready)
                AddCheck(menu, ID_ACCEPT_CAM, settings.acceptTabletCamera, L"Accept the tablet's camera as a webcam");
        }

        // The tablet's microphone as a PC microphone. Kernel driver, so it is installed on request only (it adds a recording device to
        // Windows), and loaded only while the option below is ticked: ticking it enables the driver, unticking it (and exiting PadDisplay)
        // disables it. Which virtual microphone carries it: PadDisplay's driver where Windows loads it, otherwise VB-CABLE (see micbackend.h).
        void AddMicrophoneItems(HMENU menu, const Settings &settings, const TrayMenuState &state)
        {
            const MicChoice micChoice = ChooseMicBackend(settings);
            const VddState mic = QueryMic();
            if (micChoice.backend == MicBackend::Driver && mic == VddState::NotInstalled)
            {
                if (!state.installing)
                    AddItem(menu, ID_SETUP_MIC, L"Install microphone driver for the tablet's microphone (admin)…");
            }
            else if (micChoice.backend == MicBackend::None)
            {
                if (!state.installing)
                    AddItem(menu, ID_SETUP_CABLE, L"Set up the tablet's microphone (installs VB-CABLE, admin)…");
            }
            else
            {
                AddCheck(menu, ID_ACCEPT_MIC, settings.acceptTabletMic,
                         micChoice.backend == MicBackend::Driver ? L"Accept the tablet's microphone (loads the microphone driver)"
                                                                 : L"Accept the tablet's microphone (VB-CABLE: apps choose \"CABLE Output\")");
                if (micChoice.backend == MicBackend::Cable)
                    AddItem(menu, ID_ABOUT_CABLE, L"VB-CABLE is donationware by VB-Audio (vb-cable.com)…");
            }
            if (mic != VddState::NotInstalled && !state.installing)
            {
                if (mic == VddState::Problem || state.micNeedsRepair)
                    AddItem(menu, ID_SETUP_MIC, L"Repair microphone driver (admin)…");
                AddItem(menu, ID_REMOVE_MIC, L"Remove microphone driver (admin)…");
            }
        }

        HMENU MakeRateMenu(UINT base, int current, std::initializer_list<int> rates)
        {
            HMENU sub = CreatePopupMenu();
            for (int r : rates)
                AddCheck(sub, base + r, r == current, (std::to_wstring(r) + L" Mbps").c_str());
            return sub;
        }

        HMENU MakeCodecMenu(const Settings &settings)
        {
            HMENU codec = CreatePopupMenu();
            AddCheck(codec, ID_CODEC_AUTO, settings.codec == L"auto", L"Auto (H.264 on USB, HEVC on Wi-Fi)");
            AddCheck(codec, ID_CODEC_HEVC, settings.codec == L"hevc", L"HEVC");
            AddCheck(codec, ID_CODEC_H264, settings.codec == L"h264", L"H.264");
            return codec;
        }

        void AddGpuMenu(HMENU menu, const Settings &settings)
        {
            const std::vector<EncodeGpu> gpus = ListEncodeGpus();
            if (gpus.size() <= 1)
                return;
            const auto isChosen = [&](const EncodeGpu &g)
            { return _wcsicmp(g.name.c_str(), settings.gpu.c_str()) == 0; };
            HMENU gpuMenu = CreatePopupMenu();
            AddCheck(gpuMenu, ID_GPU_AUTO, std::none_of(gpus.begin(), gpus.end(), isChosen), L"Auto (the integrated GPU when gaming or on battery, else the fastest)");
            for (size_t i = 0; i < gpus.size(); ++i)
                AddCheck(gpuMenu, ID_GPU_FIRST + UINT(i), isChosen(gpus[i]), (gpus[i].name + L" (" + EncoderName(gpus[i].vendorId) + L")").c_str());
            AddSeparator(gpuMenu);
            AddGrayed(gpuMenu, L"A new choice restarts PadDisplay once (after the tablet disconnects)");
            AddSubmenu(menu, gpuMenu, L"Encode on");
        }

        void AddStreamItems(HMENU menu, const Settings &settings)
        {
            AddSubmenu(menu, MakeRateMenu(ID_USB_RATE, settings.usbBitrateMbps, {20, 30, 40, 60, 80}), L"Max bitrate (USB)");
            AddSubmenu(menu, MakeRateMenu(ID_WIFI_RATE, settings.wifiBitrateMbps, {8, 15, 25, 35, 50}), L"Max bitrate (Wi-Fi)");
            AddSubmenu(menu, MakeCodecMenu(settings), L"Codec");
            AddGpuMenu(menu, settings);
            AddCheck(menu, ID_ADAPTIVE, settings.adaptive, L"Adaptive bitrate");
            AddCheck(menu, ID_CURSOR, settings.drawCursor, L"Show mouse cursor on tablet");
            AddCheck(menu, ID_MATCH_RES, settings.matchClientResolution, L"Match tablet resolution");
            AddCheck(menu, ID_DETACH, settings.monitorOnlyWhileConnected, L"Virtual monitor only while a tablet is connected");
            AddGrayed(menu, L"(changes apply on next connection)");
        }

        void AddHostItems(HMENU menu, const Settings &settings, bool autostart)
        {
            const bool elevated = HostIsElevated();
            AddCheck(menu, ID_AUTOSTART, autostart, L"Start with Windows");
            AddCheck(menu, ID_ADMIN, elevated, L"Run as administrator (touch and keys reach Task Manager)");
            // Only works elevated (it writes a machine policy and must inject into the elevated prompt), so only offer it then.
            if (elevated)
            {
                AddCheck(menu, ID_UAC_CLICK, settings.uacClickableOnTablet, L"Let the tablet answer UAC prompts in only-screen mode (less secure)");
                AddCheck(menu, ID_LOCK_SCREEN, settings.lockScreenControl, L"Let the tablet see and sign in to the lock screen (types your password)");
                AddCheck(menu, ID_PRESIGNIN, LogonServiceInstalled(), L"Reach the sign-in screen before anyone signs in (installs a service, Wi-Fi only)");
            }
            AddItem(menu, ID_OPEN_LOG, L"Open log");
            AddItem(menu, ID_OPEN_FOLDER, L"Open settings folder");
            AddItem(menu, ID_EXIT, L"Exit");
        }

    } // namespace

    void ShowTrayMenu(HWND owner, const Settings &settings, const TrayMenuState &state)
    {
        HMENU menu = CreatePopupMenu();
        AddStatusLines(menu, settings, state);
        AddMonitorDriverItem(menu, settings, state);
        AddGamepadItem(menu, state.installing);
        AddCameraItem(menu, settings, state.installing);
        AddMicrophoneItems(menu, settings, state);
        AddSeparator(menu);
        AddStreamItems(menu, settings);
        AddSeparator(menu);
        AddHostItems(menu, settings, state.autostart);

        POINT pt;
        GetCursorPos(&pt);
        SetForegroundWindow(owner);
        TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, owner, nullptr);
        DestroyMenu(menu);
    }

    bool IsGpuChoice(UINT id) { return id == ID_GPU_AUTO || (id >= ID_GPU_FIRST && id < ID_GPU_FIRST + kMaxGpuChoices); }

    void ApplyMenuChoice(UINT id, Settings &settings)
    {
        switch (id)
        {
        case ID_ACCEPT_MIC:
            settings.acceptTabletMic = !settings.acceptTabletMic;
            return;
        case ID_ACCEPT_CAM:
            settings.acceptTabletCamera = !settings.acceptTabletCamera;
            return;
        case ID_ADAPTIVE:
            settings.adaptive = !settings.adaptive;
            return;
        case ID_UAC_CLICK:
            settings.uacClickableOnTablet = !settings.uacClickableOnTablet;
            return;
        case ID_LOCK_SCREEN:
            settings.lockScreenControl = !settings.lockScreenControl;
            return;
        case ID_CURSOR:
            settings.drawCursor = !settings.drawCursor;
            return;
        case ID_MATCH_RES:
            settings.matchClientResolution = !settings.matchClientResolution;
            return;
        case ID_DETACH:
            settings.monitorOnlyWhileConnected = !settings.monitorOnlyWhileConnected;
            return;
        case ID_CODEC_AUTO:
            settings.codec = L"auto";
            return;
        case ID_CODEC_HEVC:
            settings.codec = L"hevc";
            return;
        case ID_CODEC_H264:
            settings.codec = L"h264";
            return;
        case ID_GPU_AUTO:
            settings.gpu = L"auto";
            return;
        }
        if (IsGpuChoice(id))
        {
            const std::vector<EncodeGpu> gpus = ListEncodeGpus();
            if (id - ID_GPU_FIRST < gpus.size())
                settings.gpu = gpus[id - ID_GPU_FIRST].name;
        }
        else if (id > ID_WIFI_RATE && id < ID_WIFI_RATE + 100)
        {
            settings.wifiBitrateMbps = int(id - ID_WIFI_RATE);
        }
        else if (id > ID_USB_RATE && id < ID_USB_RATE + 100)
        {
            settings.usbBitrateMbps = int(id - ID_USB_RATE);
        }
    }

} // namespace pd
