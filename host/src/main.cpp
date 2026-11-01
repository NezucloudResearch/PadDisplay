// PadDisplay by Nezucloud. Copyright (c) 2026 Nezucloud.
// SPDX-License-Identifier: MIT
// Host: tray app that streams the virtual monitor to the Android tablet.
//   PadDisplay.exe                 run in the tray
//   PadDisplay.exe --console       same, with a log console
//   PadDisplay.exe --list          print displays/adapters and exit
//   PadDisplay.exe --install-driver
//                                  (elevated) install or enable the bundled virtual monitor driver
//   PadDisplay.exe --install-vdd | --install-vigem | --install-camera | --install-cable | --trust-publishers |
//                  --install-autostart | --uninstall-setup   [--append-log]
//                                  (elevated) one step each, for the installer and the uninstaller: see kSetupCommands
//   PadDisplay.exe --dump out.h265 [--seconds 10] [--codec h264|hevc] [--size 2560x1600]
//                                  capture+encode the virtual display to a raw stream file (no tablet needed)
#include "commandline.h"
#include "crashdump.h"
#include "display/display.h"
#include "display/realscreens.h"
#include "display/securedesktop.h"
#include "log.h"
#include "net/net.h"
#include "realtime.h"
#include "secure/service.h"
#include "secure/uacrelay.h"
#include "session/server.h"
#include "settings.h"
#include "setup/setup_commands.h"
#include "setup/vdd.h"
#include "text.h"
#include "tray/gpu_preference.h"
#include "tray/instance.h"
#include "tray/tray.h"

#include <windows.h>
#include <mfapi.h>
#include <shellapi.h>
#include <cstdio>
#include <string>

using namespace pd;

namespace
{

    struct HelperRole
    {
        const wchar_t *flag;
        int (*run)();
    };

    // None of these is started by the user. --uac-relay and --uac-inject are the UAC-click helper, run as SYSTEM by the scheduled task
    // (see uacrelay.*). --service is the LocalSystem service (SCM entry point) for pre-sign-in access, and --logon-host the per-session
    // worker it launches as SYSTEM on the Winlogon desktop. Each does its one job and exits, well before any of the normal start-up.
    const HelperRole kHelperRoles[] = {
        {L"--uac-relay", RunUacRelayBootstrap},
        {L"--uac-inject", RunUacInjector},
        {L"--service", RunLogonService},
        {L"--logon-host", RunLogonHost},
    };

    const HelperRole *FindHelperRole(const CommandLine &args)
    {
        for (const HelperRole &role : kHelperRoles)
            if (args.Has(role.flag))
                return &role;
        return nullptr;
    }

    int RunList(const Settings &settings)
    {
        LogDisplays();
        VirtualDisplay vd;
        if (FindVirtualDisplay(settings.displayOverride, vd))
        {
            LOGI("virtual display: %ls (%ls) attached=%d", vd.gdiName.c_str(), vd.adapterName.c_str(), vd.attached);
            for (auto &m : ListModes(vd.gdiName))
                LOGI("    mode %dx%d@%d", m.width, m.height, m.hz);
        }
        else
        {
            LOGI("virtual display: not found");
        }
        return 0;
    }

    int RunDump(const wchar_t *path, const CommandLine &args, const Settings &settings)
    {
        ClientHello h;
        h.version = kProtocolVersion;
        h.screenW = 2560;
        h.screenH = 1600;
        h.codecMask = MaskH264 | MaskHEVC;
        strcpy_s(h.name, "dump");
        if (auto size = args.ValueOf(L"--size"))
            swscanf_s(size, L"%hux%hu", &h.screenW, &h.screenH);
        Settings s = settings;
        if (s.gamingMode)
            h.flags |= HelloGamingMode;
        if (auto c = args.ValueOf(L"--codec"))
            s.codec = c;
        int seconds = 10;
        if (auto sec = args.ValueOf(L"--seconds"))
            seconds = _wtoi(sec);
        if (auto fps = args.ValueOf(L"--fps"))
            h.maxFps = uint16_t(_wtoi(fps));

        FILE *f = nullptr;
        if (_wfopen_s(&f, path, L"wb") || !f)
        {
            LOGI("cannot open %ls", path);
            return 1;
        }
        Session session(Conn{}, h, true, s, [](const std::wstring &st)
                        { LOGI("%s", Narrow(st).c_str()); });
        session.Run(f, seconds);
        long size = ftell(f);
        fclose(f);
        LOGI("dump: wrote %ld bytes to %ls", size, path);
        return size > 0 ? 0 : 1;
    }

    void RecoverFromCrash()
    {
        // A crash while a tablet was the PC's only screen leaves the real screens off: bring them back.
        if (SnapshotRealDisplays().empty() && !LoadHomeLayout().empty())
        {
            LOGI("no real screen is on: restoring the stored display setup");
            if (ShowOnAllAgain())
                RestoreRealScreens();
        }
        RestoreSecureDesktop(); // a crash in only-screen UAC mode could have left the secure desktop disabled
    }

    void WaitForVirtualDisplay()
    {
        VirtualDisplay vd;
        for (int i = 0; i < 30 && !FindVirtualDisplay(L"", vd); ++i)
            Sleep(100);
    }

    void SwitchMonitorOffUntilATabletConnects()
    {
        const bool off = SetVirtualMonitorEnabled(false, 5000);
        LOGI("virtual monitor: switched off until a tablet connects -> %s", off ? "ok" : "not possible (no on/off tasks: run the admin setup)");
        if (off)
            RememberRealScreens();
    }

    struct MonitorReady
    {
        bool restarted = false; // a new GPU preference needs a new process, and one is on its way
        VddState driver = VddState::NotInstalled;
    };

    MonitorReady PrepareVirtualMonitor(const Settings &settings, SingleInstance &single)
    {
        // No virtual monitor: the screens are as the user has them now. Remembered at every start, because they may have been
        // re-arranged since the last run, and a layout from back then must never be forced onto them.
        if (QueryVdd() != VddState::Ready)
            RememberRealScreens();
        // The virtual monitor stays off while no tablet is connected (it is switched on for a session and off again after it).
        // A monitor that is on now was left by a crash, an older version or the instance that handed over to this one: switch it
        // off, which also puts the screens back as the user had them. The one thing that needs it on is the first start: which
        // GPU renders it decides this program's GPU preference, which Windows reads at process start (see EnsureGpuPreference).
        const bool autoDetect = settings.displayOverride.empty();
        const bool idleOff = settings.monitorOnlyWhileConnected && autoDetect;
        if (autoDetect && (!idleOff || !GpuPreferenceStored()) && QueryVdd() == VddState::Disabled && SetVirtualMonitorEnabled(true, 5000))
            WaitForVirtualDisplay();
        VirtualDisplay vd;
        const bool haveVd = FindVirtualDisplay(settings.displayOverride, vd);
        if (EnsureGpuPreference(settings, haveVd && vd.attached ? vd.gdiName : std::wstring()))
        {
            single.Release();
            if (RestartSameCommandLine())
                return {true};
        }
        if (idleOff && QueryVdd() == VddState::Ready)
            SwitchMonitorOffUntilATabletConnects();
        const VddState driver = QueryVdd();
        if (!haveVd && !(idleOff && driver == VddState::Disabled))
            LOGI("warning: no virtual display found (driver %ls)", VddStateText(driver));
        return {false, driver};
    }

    // "Run as administrator" is on: start again with the rights (one UAC prompt, none when the sign-in task did it). true = that one takes over.
    bool RestartWithAdminRights(SingleInstance &single)
    {
        LOGI("administrator mode is on: restarting with administrator rights");
        if (StartOtherInstance(true))
        {
            single.Release();
            return true;
        }
        LOGI("not elevated (cancelled): input cannot reach administrator windows this time");
        return false;
    }

    const wchar_t *LogFileName(bool setup, bool console) { return setup ? L"\\install-driver.log" : console ? L"\\host-cli.log"
                                                                                                            : L"\\host.log"; }

    // Static: worker threads and the exit watchdog can still be running when wWinMain returns.
    Settings g_settings;
    SingleInstance g_single;

    int RunTray(HINSTANCE instance, VddState driver)
    {
        static Tray tray(g_settings, g_single);
        if (!tray.Start(instance, driver))
            return 1;
        tray.RunUntilExit();
        MFShutdown();
        if (tray.RestartWanted())
        {
            g_single.Release();
            RestartSameCommandLine();
        }
        return 0;
    }

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int)
{
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    DisableProcessPowerThrottling();
    CommandLine args;
    args.argv = CommandLineToArgvW(GetCommandLineW(), &args.argc);
    if (const HelperRole *role = FindHelperRole(args))
        return role->run();
    const wchar_t *dumpPath = args.ValueOf(L"--dump");
    const bool list = args.Has(L"--list");
    const SetupCommand *setup = FindSetupCommand(args);
    const bool tray = !setup && !dumpPath && !list;

    // Before LogInit: a second copy must not write into the running one's log.
    if (tray && !g_single.Acquire())
    {
        MessageBoxW(nullptr, L"PadDisplay is already running (see the tray).", L"PadDisplay", MB_ICONINFORMATION);
        return 0;
    }

    // The tray host appends: its log then also holds the run before (a crash, the restart for administrator rights).
    LogInit(AppDataDir() + LogFileName(setup != nullptr, dumpPath || list), args.Has(L"--console") || list || dumpPath, tray || (setup && args.Has(L"--append-log")));
    if (tray)
        InstallCrashHandler(AppDataDir());
    g_settings.Load();
    if (setup)
        return setup->run(g_settings);
    if (g_single.Held() && g_settings.runAsAdmin && !HostIsElevated() && RestartWithAdminRights(g_single))
        return 0;
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION, MFSTARTUP_LITE);
    NetStartup();
    if (list)
        return RunList(g_settings);

    RecoverFromCrash();
    const MonitorReady monitor = PrepareVirtualMonitor(g_settings, g_single);
    if (monitor.restarted)
        return 0;
    if (dumpPath)
        return RunDump(dumpPath, args, g_settings);
    return RunTray(instance, monitor.driver);
}
