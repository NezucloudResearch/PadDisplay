// PadDisplay by Nezucloud. Copyright (c) 2026 Nezucloud.
// SPDX-License-Identifier: MIT
#include "tray.h"

#include "audio/cablesink.h"
#include "autostart.h"
#include "camera/vcam.h"
#include "display/display.h"
#include "display/realscreens.h"
#include "display/securedesktop.h"
#include "encoder/gpu_profile.h"
#include "exepath.h"
#include "gpu_preference.h"
#include "instance.h"
#include "log.h"
#include "mic/micbackend.h"
#include "secure/service.h"
#include "session/server.h"
#include "traymenu.h"

#include <wtsapi32.h>
#include <cstdint>
#include <thread>

namespace pd
{
    namespace
    {

        constexpr UINT WM_TRAY = WM_APP + 1;
        constexpr UINT WM_STATUS = WM_APP + 2;
        constexpr UINT WM_DRIVER_DONE = WM_APP + 3; // wParam = success
        constexpr UINT WM_MIC_SETUP = WM_APP + 4;   // wParam 1 = ask now (the user ticked the option), 0 = the tablet asked: a balloon first
        constexpr UINT WM_GPU_RESTART = WM_APP + 5; // the tablet has left and the GPU choice is waiting to be applied

        constexpr wchar_t kWindowClass[] = L"PadDisplayTray";
        constexpr ULONGLONG kMicBalloonGapMs = 60000; // the tablet asks on every connection: do not nag

        // Only one tray exists, and the window procedure and OnMicSetupNeeded have no other way to it.
        Tray *g_tray = nullptr;

        LRESULT CALLBACK TrayWindowProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) { return g_tray->HandleMessage(hwnd, msg, wp, lp); }

        HICON MakeIcon()
        {
            const int s = 32;
            BITMAPV5HEADER bh{};
            bh.bV5Size = sizeof(bh);
            bh.bV5Width = s;
            bh.bV5Height = -s;
            bh.bV5Planes = 1;
            bh.bV5BitCount = 32;
            bh.bV5Compression = BI_BITFIELDS;
            bh.bV5RedMask = 0x00FF0000;
            bh.bV5GreenMask = 0x0000FF00;
            bh.bV5BlueMask = 0x000000FF;
            bh.bV5AlphaMask = 0xFF000000;
            void *bits = nullptr;
            HDC dc = GetDC(nullptr);
            HBITMAP color = CreateDIBSection(dc, reinterpret_cast<BITMAPINFO *>(&bh), DIB_RGB_COLORS, &bits, nullptr, 0);
            ReleaseDC(nullptr, dc);
            auto *px = static_cast<uint32_t *>(bits);
            for (int y = 0; y < s; ++y)
                for (int x = 0; x < s; ++x)
                {
                    bool body = x >= 2 && x < 30 && y >= 6 && y < 26;   // tablet body
                    bool screen = x >= 5 && x < 27 && y >= 9 && y < 23; // screen
                    px[y * s + x] = !body ? 0 : screen ? 0xFF3DA5F5u
                                                       : 0xFF263238u;
                }
            HBITMAP mask = CreateBitmap(s, s, 1, 1, nullptr);
            ICONINFO ii{TRUE, 0, 0, mask, color};
            HICON icon = CreateIconIndirect(&ii);
            DeleteObject(mask);
            DeleteObject(color);
            return icon;
        }

        // Setup steps that need the one-time admin run: on/off tasks, NVIDIA performance profile, the bundled game controller
        // driver and (Windows 11) the virtual camera.
        bool SetupIncomplete(const Settings &settings)
        {
            if (!ToggleTasksExist())
                return true;
            if (QueryVigem() == VddState::NotInstalled || QueryCamera() == CamState::NotInstalled)
                return true;
            return settings.gpuMaxPerformance && QueryNvidiaProfile() == GpuProfileState::NotSet;
        }

        // Runs "PadDisplay.exe <args>" with administrator rights (one UAC prompt) and waits for it. true = it exited with 0.
        bool RunElevatedAndWait(const wchar_t *args)
        {
            const std::wstring exe = ThisExePath();
            SHELLEXECUTEINFOW sei{sizeof(sei)};
            sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
            sei.lpVerb = L"runas";
            sei.lpFile = exe.c_str();
            sei.lpParameters = args;
            sei.nShow = SW_HIDE;
            DWORD code = 1;
            if (ShellExecuteExW(&sei) && sei.hProcess)
            {
                WaitForSingleObject(sei.hProcess, INFINITE);
                GetExitCodeProcess(sei.hProcess, &code);
                CloseHandle(sei.hProcess);
            }
            else
            {
                LOGI("driver install: elevation cancelled or failed (%lu)", GetLastError());
            }
            return code == 0;
        }

    } // namespace

    Tray::Tray(Settings &settings, SingleInstance &instance)
        : settings_(settings), instance_(instance),
          micDriver_(settings, [this](const std::wstring &s)
                     { SetStatus(s); }, [this]
                     { if (HWND hwnd = hwnd_) PostMessageW(hwnd, WM_MIC_SETUP, 1, 0); })
    {
        g_tray = this;
    }

    Tray::~Tray() = default;

    bool Tray::Start(HINSTANCE instance, VddState vddState)
    {
        server_ = std::make_unique<Server>(settings_, [this](const std::wstring &s)
                                           { SetStatus(s); });
        if (!server_->Start())
        {
            MessageBoxW(nullptr, L"PadDisplay could not listen on TCP port 27183. Is it already running?", L"PadDisplay", MB_ICONERROR);
            return false;
        }
        beacon_.Start();
        adb_.Start(settings_.adbPath);
        CreateTrayWindow(instance);
        AddIcon();
        LOGI("PadDisplay %s by Nezucloud started. PIN %u", PD_VERSION_STR, settings_.pin);
        LOGI("game controller driver (ViGEmBus): %ls", VddStateText(QueryVigem()));
        LOGI("running %s administrator rights", HostIsElevated() ? "with" : "without");
        std::thread([this]
                    { micDriver_.Apply(false); })
            .detach();
        CheckSetupInBackground();
        WarnIfMonitorDriverMissing(vddState);
        return true;
    }

    void Tray::CreateTrayWindow(HINSTANCE instance)
    {
        WNDCLASSW wc{};
        wc.lpfnWndProc = TrayWindowProc;
        wc.hInstance = instance;
        wc.lpszClassName = kWindowClass;
        RegisterClassW(&wc);
        HWND hwnd = CreateWindowW(wc.lpszClassName, L"PadDisplay", 0, 0, 0, 0, 0, nullptr, nullptr, instance, nullptr);
        hwnd_ = hwnd;
        // Explorer (normal rights) announces its restart to this window; an elevated window ignores that unless allowed.
        if (HostIsElevated())
            ChangeWindowMessageFilterEx(hwnd, RegisterWindowMessageW(L"TaskbarCreated"), MSGFLT_ALLOW, nullptr);
        // Learn when this session is locked/unlocked (WM_WTSSESSION_CHANGE on the tray thread, never a hot
        // thread), so the session can tell the tablet "the PC is locked" instead of blaming a UAC prompt.
        WTSRegisterSessionNotification(hwnd, NOTIFY_FOR_THIS_SESSION);
        icon_.cbSize = sizeof(icon_);
        icon_.hWnd = hwnd;
        icon_.uID = 1;
        icon_.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
        icon_.uCallbackMessage = WM_TRAY;
        icon_.hIcon = MakeIcon();
        wcscpy_s(icon_.szTip, L"PadDisplay – waiting for tablet");
    }

    // icon_.uFlags stays complete: changes go out on a copy, because Explorer's restart adds the icon again from icon_, and
    // one added without NIF_ICON | NIF_MESSAGE has no picture and never reports a click.
    void Tray::AddIcon() { Shell_NotifyIconW(NIM_ADD, &icon_); }

    void Tray::CheckSetupInBackground()
    {
        std::thread([this]
                    {
        // Now elevated: "Start with Windows" can start it elevated without a prompt (a task, not the Run key).
        if (HostIsElevated() && settings_.runAsAdmin && RunKeyPresent()) SetAutostart(settings_, true);
        setupIncomplete_ = SetupIncomplete(settings_) ? 1 : 0;
        LOGI("setup: %s; NVIDIA profile %s", setupIncomplete_ ? "incomplete (tray: Finish PadDisplay setup)" : "complete",
             QueryNvidiaProfile() == GpuProfileState::Set ? "max performance" : "not set"); })
            .detach();
    }

    void Tray::WarnIfMonitorDriverMissing(VddState vddState)
    {
        if (vddState == VddState::Ready || !settings_.displayOverride.empty())
            return;
        Balloon(vddState == VddState::NotInstalled ? L"Virtual monitor driver needed" : L"Virtual monitor driver is off",
                L"Click here to set it up (one administrator prompt). PadDisplay needs it to add the tablet as a second screen.",
                NIIF_WARNING);
    }

    void Tray::RunUntilExit()
    {
        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0) > 0)
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        StartExitWatchdog();
        Cleanup();
    }

    // Exit has to exit: if anything in Cleanup hangs (a session thread, the encoder), the port and the virtual
    // monitor would stay. After 15 s, take the monitor away and end the process regardless.
    void Tray::StartExitWatchdog()
    {
        std::thread([this]
                    {
        Sleep(15000);
        LOGI("exit: cleanup is taking too long, ending the process");
        if (!LeavesMonitorToTheNextInstance()) RemoveVirtualMonitor();
        RestoreSecureDesktop(); // never leave the secure desktop disabled, even on a forced exit
        TerminateProcess(GetCurrentProcess(), 0); })
            .detach();
    }

    void Tray::Cleanup()
    {
        adb_.Stop();
        beacon_.Stop();
        // Restarting (another PadDisplay takes over, or one starts after an install): the tablet reconnects to it by itself.
        if (server_)
            server_->Stop(exit_ == ExitKind::Quit ? SessionEnd::HostExit : SessionEnd::HostRestart); // a sign-out has ended it already
        server_.reset();                                                                             // ends the session, which restores the secure desktop if it had moved UAC prompts
        RestoreSecureDesktop();                                                                      // and again here in case no session was tracking it
        WaitScreenRestore(5000);                                                                     // the PC's own screens must not stay off just because a UAC prompt was up when we left
        if (exit_ == ExitKind::HandOver)
            instance_.Release(); // the other PadDisplay is waiting for this mutex and the port, which is free now
        if (LeavesMonitorToTheNextInstance())
            return;
        RemoveVirtualMonitor();
        micDriver_.DisableOnExit(); // not loaded while PadDisplay is not running
    }

    // On exit / sign-out / shutdown: take the virtual monitor away so only real displays remain.
    void Tray::RemoveVirtualMonitor()
    {
        if (monitorRemoved_ || !settings_.removeMonitorOnExit || !settings_.displayOverride.empty())
            return;
        monitorRemoved_ = true;
        if (QueryVdd() != VddState::Ready)
            return;
        if (SetVirtualMonitorEnabled(false, 4000))
            return;
        // No tasks (driver installed some other way): at least detach it from the desktop.
        VirtualDisplay vd;
        if (FindVirtualDisplay(L"", vd) && vd.attached)
            Detach(vd.gdiName);
    }

    void Tray::SetStatus(const std::wstring &status)
    {
        {
            std::lock_guard lock(statusMutex_);
            status_ = status;
        }
        if (HWND hwnd = hwnd_)
            PostMessageW(hwnd, WM_STATUS, 0, 0);
    }

    void Tray::AskForMicSetup()
    {
        if (HWND hwnd = hwnd_)
            PostMessageW(hwnd, WM_MIC_SETUP, 0, 0);
    }

    // What a click on the balloon does depends on what it talks about, and session errors arrive here as plain text.
    void Tray::Balloon(const std::wstring &title, const std::wstring &text, DWORD icon)
    {
        balloonTopic_ = text.find(L"set up the tablet's microphone") != std::wstring::npos ? BalloonTopic::Mic
                        : text.find(L"ViGEmBus") != std::wstring::npos                     ? BalloonTopic::Gamepad
                                                                                           : BalloonTopic::Other;
        NOTIFYICONDATAW n = icon_;
        n.uFlags = NIF_INFO;
        n.dwInfoFlags = icon;
        wcsncpy_s(n.szInfoTitle, title.c_str(), _TRUNCATE);
        wcsncpy_s(n.szInfo, text.c_str(), _TRUNCATE);
        Shell_NotifyIconW(NIM_MODIFY, &n);
    }

    LRESULT Tray::HandleMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
    {
        static const UINT taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
        if (msg == taskbarCreated)
        { // Explorer restarted: re-add the icon
            AddIcon();
            return 0;
        }
        switch (msg)
        {
        case WM_TRAY:
            OnTrayIcon(hwnd, LOWORD(lp));
            return 0;
        case WM_MIC_SETUP:
            OnMicSetupRequest(wp != 0);
            return 0;
        case WM_DRIVER_DONE:
            OnInstallDone(hwnd, wp != 0);
            return 0;
        case WM_STATUS:
            OnStatusChanged(hwnd);
            return 0;
        case WM_GPU_RESTART:
            if (gpuRestartPending_ && !TabletConnected())
                RestartForGpuPreference(hwnd);
            return 0;
        case WM_COMMAND:
            OnCommand(hwnd, LOWORD(wp));
            return 0;
        case WM_WTSSESSION_CHANGE:
            if (wp == WTS_SESSION_LOCK)
                SetSessionLocked(true);
            else if (wp == WTS_SESSION_UNLOCK)
                SetSessionLocked(false);
            return 0;
        case WM_QUERYENDSESSION:
            return TRUE;
        case WM_ENDSESSION:
            if (wp)
                OnEndSession();
            return 0;
        case WM_DESTROY:
            WTSUnRegisterSessionNotification(hwnd);
            Shell_NotifyIconW(NIM_DELETE, &icon_);
            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    // Windows is signing out or shutting down: this process is about to be terminated.
    void Tray::OnEndSession()
    {
        server_.reset();
        RemoveVirtualMonitor();
        micDriver_.DisableOnExit();
    }

    void Tray::OnTrayIcon(HWND hwnd, UINT mouseMessage)
    {
        if (mouseMessage == WM_RBUTTONUP || mouseMessage == WM_LBUTTONUP)
            ShowMenu(hwnd);
        else if (mouseMessage == NIN_BALLOONUSERCLICK)
            OnBalloonClicked();
    }

    void Tray::OnBalloonClicked()
    {
        if (balloonTopic_ == BalloonTopic::Mic)
        {
            PromptInstallCable();
            return;
        }
        const VddState vigem = balloonTopic_ == BalloonTopic::Gamepad ? QueryVigem() : VddState::Ready;
        if (QueryVdd() != VddState::Ready)
            StartInstall(InstallJob::Setup);
        else if (vigem != VddState::Ready)
            StartInstall(InstallJob::Gamepad); // installs the bundled one when it is missing
    }

    void Tray::OnMicSetupRequest(bool askNow)
    {
        if (askNow)
        {
            PromptInstallCable();
            return;
        }
        const ULONGLONG now = GetTickCount64();
        if (micBalloonShownAt_ != 0 && now - micBalloonShownAt_ <= kMicBalloonGapMs)
            return;
        micBalloonShownAt_ = now;
        Balloon(L"The tablet's microphone", L"The tablet wants to use its microphone, and this PC has no virtual microphone yet. Click here to set up the tablet's microphone.");
    }

    void Tray::OnStatusChanged(HWND hwnd)
    {
        std::wstring status;
        {
            std::lock_guard lock(statusMutex_); // a copy, because Shell_NotifyIcon waits for Explorer and the stream thread waits for this lock
            status = status_;
        }
        NOTIFYICONDATAW tip = icon_;
        tip.uFlags = NIF_TIP;
        wcsncpy_s(tip.szTip, (L"PadDisplay – " + status).c_str(), _TRUNCATE);
        wcscpy_s(icon_.szTip, tip.szTip);
        Shell_NotifyIconW(NIM_MODIFY, &tip);
        if (status.rfind(L"Error: ", 0) == 0)
            Balloon(L"PadDisplay", status.substr(7), NIIF_WARNING);
        if (gpuRestartPending_ && status == L"Waiting for tablet")
            PostMessageW(hwnd, WM_GPU_RESTART, 0, 0); // the tablet has left
    }

    bool Tray::TabletConnected() const { return server_ && server_->TabletConnected(); }

    void Tray::ShowMenu(HWND hwnd)
    {
        TrayMenuState state;
        {
            std::lock_guard lock(statusMutex_);
            state.status = status_;
        }
        state.tlsFingerprint = server_ ? server_->TlsFingerprint() : std::string();
        state.installing = installing_;
        state.setupIncomplete = setupIncomplete_;
        state.micNeedsRepair = micDriver_.NeedsRepair();
        state.autostart = AutostartEnabled(settings_);
        ShowTrayMenu(hwnd, settings_, state);
    }

    void Tray::OnCommand(HWND hwnd, UINT id)
    {
        switch (id)
        {
        case ID_EXIT:
            DestroyWindow(hwnd);
            return;
        case ID_OPEN_LOG:
            ShellExecuteW(nullptr, L"open", LogPath().c_str(), nullptr, nullptr, SW_SHOW);
            return;
        case ID_OPEN_FOLDER:
            ShellExecuteW(nullptr, L"open", AppDataDir().c_str(), nullptr, nullptr, SW_SHOW);
            return;
        case ID_ABOUT_CABLE:
            ShellExecuteW(nullptr, L"open", L"https://vb-audio.com/Cable/", nullptr, nullptr, SW_SHOWNORMAL);
            return;
        case ID_AUTOSTART:
            SetAutostart(settings_, !AutostartEnabled(settings_));
            return;
        case ID_ADMIN:
            SwitchAdminRights(hwnd);
            return;
        case ID_DRIVER:
        case ID_SETUP_CAMERA:
            StartInstall(InstallJob::Setup);
            return;
        case ID_ENABLE_VIGEM:
        case ID_GET_VIGEM:
            StartInstall(InstallJob::Gamepad);
            return;
        case ID_SETUP_MIC:
            StartInstall(InstallJob::Mic);
            return;
        case ID_REMOVE_MIC:
            StartInstall(InstallJob::RemoveMic);
            return;
        case ID_SETUP_CABLE:
            PromptInstallCable();
            return;
        case ID_PRESIGNIN:
            TogglePreSignInAccess();
            break;
        default:
            ApplyMenuChoice(id, settings_);
            break;
        }
        ApplySettings();
        if (IsGpuChoice(id))
            ApplyGpuChoice(hwnd);
        if (id == ID_ACCEPT_MIC)
            std::thread([this]
                        { micDriver_.Apply(true); })
                .detach(); // loads or unloads the driver
    }

    void Tray::ApplySettings()
    {
        settings_.Save();
        if (server_)
            server_->UpdateSettings(settings_);
        // Keep the service's machine-readable config (PIN + stream settings) in step while pre-sign-in access
        // is on, so the sign-in-screen host uses the current PIN. Needs the admin rights that wrote the file.
        if (settings_.preSignInAccess && HostIsElevated())
            SyncMachineConfig(settings_);
    }

    // Installs or removes the LocalSystem service. The host is elevated here (the item shows only then).
    void Tray::TogglePreSignInAccess()
    {
        std::wstring message;
        if (LogonServiceInstalled())
        {
            settings_.preSignInAccess = false;
            RemoveLogonService(message);
        }
        else
        {
            settings_.preSignInAccess = true;
            SyncMachineConfig(settings_);
            // Left on, the next start would move to the machine-wide TLS identity, and tablets refuse a PC whose fingerprint changed.
            if (!InstallLogonService(message))
                settings_.preSignInAccess = false;
        }
        if (!message.empty())
            Balloon(L"PadDisplay sign-in screen access", message);
    }

    // Windows blocks input from a normal program to the windows of an elevated one, so the tablet
    // cannot click in Task Manager or any "run as administrator" program until PadDisplay is elevated too.
    void Tray::SwitchAdminRights(HWND hwnd)
    {
        const bool toAdmin = !HostIsElevated();
        settings_.runAsAdmin = toAdmin;
        ApplySettings();
        if (!toAdmin && settings_.autostartTask)
            SetAutostart(settings_, true); // the task starts it elevated: back to the Run key (while we still can delete it)
        if (StartOtherInstance(toAdmin))
        {
            LOGI("restarting %s administrator rights", toAdmin ? "with" : "without");
            exit_ = ExitKind::HandOver;
            DestroyWindow(hwnd);
        }
        else
        {
            LOGI("restart cancelled or failed (%lu)", GetLastError());
            settings_.runAsAdmin = !toAdmin;
            ApplySettings();
        }
    }

    // The tray's GPU choice changed: bring the program's GPU preference in line. A restart applies it, which would cut a connected tablet
    // off, so with one connected it waits until the tablet leaves.
    void Tray::ApplyGpuChoice(HWND hwnd)
    {
        if (!EnsureGpuPreference(settings_, std::wstring()))
            return; // already right, or only one GPU
        if (TabletConnected())
        {
            gpuRestartPending_ = true;
            LOGI("the GPU choice applies after the tablet disconnects (PadDisplay restarts then)");
        }
        else
        {
            RestartForGpuPreference(hwnd);
        }
    }

    // Starts this program again, as it is (with administrator rights if it has them: no prompt then), and leaves the virtual monitor to it.
    void Tray::RestartForGpuPreference(HWND hwnd)
    {
        gpuRestartPending_ = false;
        if (StartOtherInstance(HostIsElevated()))
        {
            LOGI("restarting to apply the GPU choice");
            exit_ = ExitKind::HandOver;
            DestroyWindow(hwnd);
        }
        else
        {
            LOGI("could not restart (%lu): the GPU choice applies at the next start", GetLastError());
        }
    }

    void Tray::StartInstall(InstallJob job)
    {
        if (installing_)
            return;
        installing_ = true;
        installJob_ = job;
        const wchar_t *args = job == InstallJob::Mic         ? L"--install-mic"
                              : job == InstallJob::RemoveMic ? L"--remove-mic"
                              : job == InstallJob::Cable     ? L"--install-cable"
                                                             : L"--install-driver";
        std::thread([this, args]
                    { PostMessageW(hwnd_, WM_DRIVER_DONE, RunElevatedAndWait(args), 0); })
            .detach();
    }

    // Asks before installing VB-CABLE, a third-party driver (UI thread). Nothing is installed without this yes.
    void Tray::PromptInstallCable()
    {
        if (installing_)
            return;
        const int answer = MessageBoxW(
            hwnd_,
            L"The tablet's microphone needs a virtual microphone on this PC. PadDisplay's own microphone driver cannot be loaded here: "
            L"Windows loads kernel drivers only if Microsoft signed them.\n\n"
            L"PadDisplay comes with VB-CABLE instead, a free virtual audio cable by VB-Audio (donationware, vb-cable.com). "
            L"Afterwards choose \"CABLE Output\" as the microphone in your apps "
            L"(it also adds a playback device called \"CABLE Input\" that you can ignore).\n\nInstall VB-CABLE now?",
            L"PadDisplay – the tablet's microphone", MB_YESNO | MB_ICONQUESTION | MB_SETFOREGROUND);
        if (answer == IDYES)
            StartInstall(InstallJob::Cable);
    }

    void Tray::OnInstallDone(HWND hwnd, bool succeeded)
    {
        installing_ = false;
        setupIncomplete_ = SetupIncomplete(settings_) ? 1 : 0;
        LOGI("driver install finished: %s, driver %ls", succeeded ? "ok" : "failed", VddStateText(QueryVdd()));
        switch (installJob_)
        {
        case InstallJob::Cable:
            ReportCableInstall();
            return;
        case InstallJob::Mic:
        case InstallJob::RemoveMic:
            ReportMicInstall();
            return;
        case InstallJob::Gamepad:
            ReportGamepadInstall();
            return;
        case InstallJob::Setup:
            FinishMonitorInstall(hwnd, succeeded);
            return;
        }
    }

    void Tray::ReportCableInstall()
    {
        if (CableInstalled(settings_.micDevice))
        {
            settings_.acceptTabletMic = true; // installing it means wanting it
            ApplySettings();
            Balloon(L"VB-CABLE ready", L"In your apps choose \"CABLE Output\" as the microphone. VB-CABLE is donationware by VB-Audio: vb-cable.com");
        }
        else
        {
            Balloon(L"VB-CABLE not ready", L"VB-CABLE was not installed, or Windows needs a restart to finish it. See the log (tray menu → Open log).", NIIF_WARNING);
        }
    }

    void Tray::ReportMicInstall()
    {
        const VddState mic = QueryMic();
        if (mic == VddState::Ready)
            micDriver_.MarkRepaired();
        if (installJob_ == InstallJob::RemoveMic)
        {
            Balloon(L"Microphone driver removed", L"The tablet's microphone is no longer offered to PC apps.");
        }
        else if (mic == VddState::Ready)
        {
            // Installing it means wanting it: the driver is loaded while this option is on.
            settings_.acceptTabletMic = true;
            ApplySettings();
            Balloon(L"Microphone driver ready", L"Choose \"PadDisplay Virtual Microphone\" as the microphone in your PC apps. Untick \"Accept the tablet's microphone\" in this menu to unload the driver.");
        }
        else
        {
            Balloon(L"Microphone driver not enabled", std::wstring(L"The driver is ") + VddStateText(mic) + L". See the log (tray menu → Open log).", NIIF_WARNING);
        }
    }

    void Tray::ReportGamepadInstall()
    {
        const VddState vigem = QueryVigem();
        if (vigem == VddState::Ready)
            Balloon(L"Game controller driver ready", L"Controllers connected to the tablet now work on the PC.");
        else
            Balloon(L"Game controller driver not enabled",
                    std::wstring(L"ViGEmBus is ") + VddStateText(vigem) + L". See the log (tray menu → Open log).", NIIF_WARNING);
    }

    void Tray::FinishMonitorInstall(HWND hwnd, bool succeeded)
    {
        const VddState vdd = QueryVdd();
        if (!succeeded || vdd != VddState::Ready)
        {
            Balloon(L"Virtual monitor driver not installed",
                    std::wstring(L"The driver is ") + VddStateText(vdd) + L". See the log (tray menu → Open log).", NIIF_WARNING);
            return;
        }
        SetVddFailed(false); // fixed: the tray goes back to its usual items
        Balloon(L"Virtual monitor ready", L"Connect the tablet to start using it as a second screen.");
        // Hybrid laptops: Desktop Duplication needs this process on the GPU that renders the new
        // monitor, and that preference only applies at start-up.
        VirtualDisplay vd;
        if (FindVirtualDisplay(settings_.displayOverride, vd) && vd.attached && EnsureGpuPreference(settings_, vd.gdiName))
        {
            exit_ = ExitKind::RestartAfterInstall;
            DestroyWindow(hwnd); // the new instance switches the monitor off at its start
        }
        else if (settings_.monitorOnlyWhileConnected && settings_.displayOverride.empty())
        {
            // The install left it on. Off again until a tablet connects (a few seconds: not on this thread).
            std::thread([]
                        { SetVirtualMonitorEnabled(false, 5000); })
                .detach();
        }
    }

} // namespace pd

// The tablet wants its microphone and there is no virtual microphone to carry it (called from any thread; see micbackend.h).
void pd::OnMicSetupNeeded()
{
    if (g_tray)
        g_tray->AskForMicSetup();
}
