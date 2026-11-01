// PadDisplay by Nezucloud. Copyright (c) 2026 Nezucloud.
// SPDX-License-Identifier: MIT
// The tray program: the icon and its menu, the server behind them, and everything that has to happen at Exit.
// Runs on the UI thread, which waits on slow things (a menu, a UAC prompt, driver queries). No hot thread waits on it:
// SetStatus takes one short lock and posts a message.
#pragma once
#include "micdriver.h"
#include "net/adb.h"
#include "net/discovery.h"
#include "settings.h"
#include "setup/vdd.h"

#include <windows.h>
#include <shellapi.h>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>

namespace pd
{

    class Server;
    class SingleInstance;

    class Tray
    {
    public:
        Tray(Settings &settings, SingleInstance &instance);
        ~Tray();

        bool Start(HINSTANCE instance, VddState vddState); // false = the port is taken, and the user has been told
        void RunUntilExit();
        bool RestartWanted() const { return exit_ == ExitKind::RestartAfterInstall; }

        void SetStatus(const std::wstring &status); // any thread
        void AskForMicSetup();                      // any thread

        LRESULT HandleMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

    private:
        enum class ExitKind
        {
            Quit,
            HandOver,
            RestartAfterInstall
        };
        enum class InstallJob
        {
            Setup,
            Gamepad,
            Mic,
            RemoveMic,
            Cable
        };
        enum class BalloonTopic
        {
            Other,
            Gamepad,
            Mic
        };

        void CreateTrayWindow(HINSTANCE instance);
        void AddIcon();
        void CheckSetupInBackground();
        void WarnIfMonitorDriverMissing(VddState vddState);
        void StartExitWatchdog();
        void Cleanup();
        bool LeavesMonitorToTheNextInstance() const { return exit_ != ExitKind::Quit; }
        void RemoveVirtualMonitor();

        void Balloon(const std::wstring &title, const std::wstring &text, DWORD icon = NIIF_INFO);
        void OnTrayIcon(HWND hwnd, UINT mouseMessage);
        void OnBalloonClicked();
        void OnMicSetupRequest(bool askNow);
        void OnStatusChanged(HWND hwnd);
        void OnCommand(HWND hwnd, UINT id);
        void OnEndSession();

        bool TabletConnected() const;
        void ShowMenu(HWND hwnd);
        void ApplySettings();
        void TogglePreSignInAccess();
        void SwitchAdminRights(HWND hwnd);
        void ApplyGpuChoice(HWND hwnd);
        void RestartForGpuPreference(HWND hwnd);

        void StartInstall(InstallJob job);
        void PromptInstallCable();
        void OnInstallDone(HWND hwnd, bool succeeded);
        void ReportCableInstall();
        void ReportMicInstall();
        void ReportGamepadInstall();
        void FinishMonitorInstall(HWND hwnd, bool succeeded);

        Settings &settings_;
        SingleInstance &instance_;
        std::unique_ptr<Server> server_;
        DiscoveryBeacon beacon_;
        AdbReverser adb_;
        MicDriverSwitch micDriver_;

        std::mutex statusMutex_; // the stream thread takes this once a second: never held across anything slow
        std::wstring status_ = L"Waiting for tablet";
        std::atomic<HWND> hwnd_{nullptr};
        NOTIFYICONDATAW icon_{};

        bool installing_ = false;
        InstallJob installJob_ = InstallJob::Setup;
        BalloonTopic balloonTopic_ = BalloonTopic::Other;
        ULONGLONG micBalloonShownAt_ = 0;
        ExitKind exit_ = ExitKind::Quit;
        bool monitorRemoved_ = false;
        bool gpuRestartPending_ = false;       // the GPU choice changed while a tablet was connected: restart (to apply it) once it has left
        std::atomic<int> setupIncomplete_{-1}; // -1 unknown (checked on a worker thread), 0 complete, 1 needs the admin run
    };

} // namespace pd
