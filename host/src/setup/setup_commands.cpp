// PadDisplay by Nezucloud. Copyright (c) 2026 Nezucloud.
// SPDX-License-Identifier: MIT
#include "setup_commands.h"

#include "audio/cablesink.h"
#include "camera/vcam.h"
#include "display/securedesktop.h"
#include "encoder/gpu_profile.h"
#include "exepath.h"
#include "log.h"
#include "secure/service.h"
#include "secure/uacrelay.h"
#include "setup/setup.h"
#include "setup/vdd.h"
#include "text.h"
#include "tray/autostart.h"
#include "tray/gpu_preference.h"

#include <string>

namespace pd
{
    namespace
    {

        int Done(const char *command, bool ok, const std::wstring &message)
        {
            LOGI("%s: %s", command, Narrow(message).c_str());
            return ok ? 0 : 1;
        }

        int RunInstallMonitor(Settings &settings)
        {
            std::wstring message, gpu;
            const bool ok = InstallVdd(ThisExeDir() + L"\\driver\\vdd", message);
            ConfigureNvidiaProfile(settings.gpuMaxPerformance, gpu); // not fatal: streaming still works without it
            return Done("install-vdd", ok, message);
        }

        int RunInstallGamepad(Settings &)
        {
            std::wstring message;
            if (QueryVigem() == VddState::NotInstalled)
            {
                InstallVigemBus(ThisExeDir() + L"\\driver\\vigem", message);
                LOGI("install-vigem: %s", Narrow(message).c_str());
            }
            const bool ok = SetupVigem(message); // switches the bus on if it is off and registers the task that does so later
            return Done("install-vigem", ok, message);
        }

        int RunInstallCamera(Settings &)
        {
            std::wstring message;
            const bool ok = InstallCamera(ThisExeDir() + L"\\driver\\camera", message);
            return Done("install-camera", ok, message);
        }

        int RunRemoveCamera(Settings &)
        {
            std::wstring message;
            const bool ok = RemoveCamera(message);
            return Done("remove-camera", ok, message);
        }

        // The tray's "Finish PadDisplay setup": everything the tablet needs except a microphone.
        int RunInstallDriver(Settings &settings)
        {
            const int monitor = RunInstallMonitor(settings);
            // Not fatal: only game controllers need ViGEmBus, and only the tablet's webcam needs the virtual camera.
            RunInstallGamepad(settings);
            if (QueryCamera() == CamState::NotInstalled)
                RunInstallCamera(settings);
            return monitor;
        }

        int RunInstallMic(Settings &)
        {
            std::wstring message;
            const bool ok = InstallMicDriver(ThisExeDir() + L"\\driver\\mic", message);
            return Done("install-mic", ok, message);
        }

        int RunRemoveMic(Settings &)
        {
            std::wstring message;
            const bool ok = RemoveMicDriver(message);
            return Done("remove-mic", ok, message);
        }

        int RunInstallCable(Settings &)
        {
            std::wstring message;
            const bool ok = InstallCable(ThisExeDir() + L"\\driver\\vbcable", message);
            return Done("install-cable", ok, message);
        }

        int RunTrustPublishers(Settings &)
        {
            std::wstring message;
            const bool ok = TrustDriverPublishers(ThisExeDir() + L"\\driver", message);
            return Done("trust-publishers", ok, message);
        }

        int RunUntrustPublishers(Settings &)
        {
            std::wstring message;
            const bool ok = UntrustDriverPublishers(message);
            return Done("untrust-publishers", ok, message);
        }

        // The installer's "start with administrator rights when I sign in": the tray's Run as administrator plus Start with Windows.
        int RunInstallAutostart(Settings &settings)
        {
            if (!CreateAutostartTask())
                return Done("install-autostart", false, L"The sign-in task could not be registered.");
            settings.runAsAdmin = true;
            settings.autostartTask = true;
            settings.Save();
            SetRunKey(false);
            return Done("install-autostart", true, L"PadDisplay starts with administrator rights at sign-in, without a prompt.");
        }

        int RunRemoveAutostart(Settings &settings)
        {
            SetAutostart(settings, false);
            return Done("remove-autostart", !settings.autostartTask, settings.autostartTask ? L"The sign-in task could not be deleted." : L"PadDisplay no longer starts at sign-in.");
        }

        // The uninstaller: takes away what PadDisplay added to this PC. Drivers that were on the PC before PadDisplay stay.
        // A step that fails does not stop the others.
        int RunUninstallSetup(Settings &settings)
        {
            bool ok = true;
            std::wstring message;
            const auto step = [&](const char *what, bool done)
            {
                LOGI("uninstall-setup: %s: %s", what, Narrow(message).c_str());
                message.clear();
                ok = ok && done;
            };
            DeleteAutostartTask();
            SetRunKey(false);
            if (settings.autostartTask)
            { // the user may keep the settings: they must not claim a task that is gone
                settings.autostartTask = false;
                settings.Save();
            }
            ForgetGpuPreference();
            RestoreSecureDesktop();
            if (QueryCamera() == CamState::Ready)
                step("virtual camera", RemoveCamera(message));
            if (QueryMic() != VddState::NotInstalled)
                step("microphone driver", RemoveMicDriver(message));
            if (AddedByPadDisplay(kAddedMonitor))
                step("virtual monitor", RemoveVdd(message));
            if (AddedByPadDisplay(kAddedVigem))
                step("game controller driver", RemoveVigem(message));
            if (AddedByPadDisplay(kAddedCable))
                step("VB-CABLE", RemoveCable(ThisExeDir() + L"\\driver\\vbcable", message));
            RemoveToggleTasks();
            RemoveUacRelayTask();
            if (LogonServiceInstalled())
                step("sign-in-screen service", RemoveLogonService(message));
            DeleteTaskFolder();
            step("driver publishers", UntrustDriverPublishers(message));
            if (QueryNvidiaProfile() == GpuProfileState::Set)
                step("NVIDIA profile", ConfigureNvidiaProfile(false, message));
            ForgetSetupRecords();
            return ok ? 0 : 1;
        }

        // Pre-sign-in access (piece 4): install/remove the LocalSystem service. Run elevated (the tray/installer
        // elevates). Install also writes the machine-readable config the service needs (PIN + stream settings).
        int RunInstallService(Settings &settings)
        {
            settings.preSignInAccess = true;
            settings.Save();
            SyncMachineConfig(settings);
            std::wstring message;
            const bool ok = InstallLogonService(message);
            LOGI("setup: install sign-in-screen service %s %ls", ok ? "ok" : "FAILED", message.c_str());
            if (!ok)
            { // left on, the next start would move to the machine-wide TLS identity and tablets would refuse the PC
                settings.preSignInAccess = false;
                settings.Save();
            }
            return ok ? 0 : 1;
        }

        int RunRemoveService(Settings &settings)
        {
            settings.preSignInAccess = false;
            settings.Save();
            std::wstring message;
            const bool ok = RemoveLogonService(message);
            LOGI("setup: remove sign-in-screen service %s %ls", ok ? "ok" : "FAILED", message.c_str());
            return ok ? 0 : 1;
        }

        const SetupCommand kSetupCommands[] = {
            {L"--install-driver", RunInstallDriver},
            {L"--install-vdd", RunInstallMonitor},
            {L"--install-vigem", RunInstallGamepad},
            {L"--install-camera", RunInstallCamera},
            {L"--remove-camera", RunRemoveCamera},
            {L"--install-mic", RunInstallMic},
            {L"--remove-mic", RunRemoveMic},
            {L"--install-cable", RunInstallCable},
            {L"--trust-publishers", RunTrustPublishers},
            {L"--untrust-publishers", RunUntrustPublishers},
            {L"--install-autostart", RunInstallAutostart},
            {L"--remove-autostart", RunRemoveAutostart},
            {L"--install-service", RunInstallService},
            {L"--remove-service", RunRemoveService},
            {L"--uninstall-setup", RunUninstallSetup},
        };

    } // namespace

    const SetupCommand *FindSetupCommand(const CommandLine &args)
    {
        for (const SetupCommand &command : kSetupCommands)
            if (args.Has(command.flag))
                return &command;
        return nullptr;
    }

} // namespace pd
