// PadDisplay by Nezucloud. Copyright (c) 2026 Nezucloud.
// SPDX-License-Identifier: MIT
#include "session.h"
#include "display/display.h"
#include "display/realscreens.h"
#include "display/securedesktop.h"
#include "encoder/gpu_select.h"
#include "log.h"
#include "monitor_off.h"
#include "realtime.h"
#include "setup/vdd.h"
#include "text.h"

#include <qos2.h>
#include <algorithm>
#include <chrono>
#include <cwchar>
#include <optional>

namespace pd
{
    namespace
    {

        constexpr uint64_t kPingIntervalUs = 500000;
        constexpr uint64_t kSilenceLimitUs = 10000000; // the tablet stopped answering
        constexpr int kPipelineRetries = 150;          // about 30 s, for a mode switch
        constexpr DWORD kMonitorSwitchOnMs = 15000;

    } // namespace
    Session::Session(Conn conn, const ClientHello &hello, bool usb, const Settings &settings, StatusFn status)
        : conn_(std::move(conn)), hello_(hello), usb_(usb), settings_(settings), status_(std::move(status)) {}

    std::vector<uint8_t> ErrorPayload(ErrorCode code, const char *text)
    {
        std::vector<uint8_t> p(1 + strlen(text));
        p[0] = code;
        memcpy(p.data() + 1, text, p.size() - 1);
        return p;
    }

    void SendErrorMessage(Conn &conn, ErrorCode code, const char *text)
    {
        const std::vector<uint8_t> p = ErrorPayload(code, text);
        SendMsg(conn, Msg::Error, p.data(), uint32_t(p.size()));
    }

    WaitingPing::WaitingPing(Conn &conn, std::mutex &sendMutex)
    {
        thread_ = std::thread([this, &conn, &sendMutex]
                              {
        std::unique_lock lock(mutex_);
        while (!done_.wait_for(lock, std::chrono::milliseconds(500), [this] { return stop_; })) {
            const uint8_t ping[16] = {}; // host time 0: the answer says nothing about the round trip (see Msg::Pong)
            std::lock_guard send(sendMutex);
            if (!SendMsg(conn, Msg::Ping, ping, sizeof(ping))) return; // the tablet left; the session notices by itself
        } });
    }

    WaitingPing::~WaitingPing()
    {
        {
            std::lock_guard lock(mutex_);
            stop_ = true;
        }
        done_.notify_one();
        thread_.join();
    }

    Session::~Session()
    {
        Stop(SessionEnd::Failed); // Run has ended by now, with its own reason; this only makes sure the threads are told
        if (recvThread_.joinable())
            recvThread_.join();
        if (inputThread_.joinable())
            inputThread_.join();
        if (layoutThread_.joinable())
            layoutThread_.join();
        gamepads_.reset(); // its rumble threads send on the socket
        if (qos_)
        {
            if (qosFlow_)
                QOSRemoveSocketFromFlow(qos_, conn_.s, qosFlow_, 0);
            QOSCloseHandle(qos_);
        }
        if (conn_.s != INVALID_SOCKET)
            closesocket(conn_.s);
    }

    void Session::Stop(SessionEnd why)
    {
        EndAs(why);
        if (conn_.s == INVALID_SOCKET)
        {
            stop_ = true;
            return;
        }
        if (why == SessionEnd::HostExit || why == SessionEnd::Replaced)
            SendGoodbye(why);
        stop_ = true;
        CutOff(conn_.s); // a session thread may be inside a send to a tablet that stopped reading
    }
    // Tells the tablet that the session was ended on purpose, so it does not try to reconnect (two tablets would otherwise take
    // the PC from each other in turn). Runs on the thread that stops the session, never a session thread. Skipped when the link
    // is stalled and a session thread is stuck in a send: the tablet then finds out by itself.
    void Session::SendGoodbye(SessionEnd why)
    {
        std::unique_lock lock(sendMutex_, std::defer_lock);
        for (int i = 0; i < 20 && !lock.try_lock(); ++i)
            Sleep(10); // a frame being sent takes well under that
        if (!lock.owns_lock())
            return;
        DWORD timeout = 500;
        setsockopt(conn_.s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<char *>(&timeout), sizeof(timeout));
        const bool exiting = why == SessionEnd::HostExit;
        const char *text = exiting ? "PadDisplay was closed on the PC." : "Another tablet connected to this PC.";
        SendErrorMessage(conn_, exiting ? ErrHostClosed : ErrReplaced, text);
    }
    bool Session::Send(Msg type, const void *payload, uint32_t len)
    {
        if (dump_)
        {
            auto *p = static_cast<const uint8_t *>(payload);
            if (type == Msg::Config)
                fwrite(p, 1, len, dump_);
            if (type == Msg::Frame && len > 9)
                fwrite(p + 9, 1, len - 9, dump_);
            return true;
        }
        if (conn_.s == INVALID_SOCKET)
            return false;
        std::lock_guard lock(sendMutex_);
        if (SendMsg(conn_, type, payload, len))
            return true;
        EndAs(SessionEnd::Lost);
        return false;
    }

    void Session::SendNotice(const char *text)
    {
        Send(Msg::Notice, text, uint32_t(strlen(text)));
        if (status_)
            status_(L"Error: " + Widen(text));
    }

    void Session::SendLockState(bool frozen)
    {
        if (hello_.version < 5)
            return; // older tablets have no password box to offer
        const int state = frozen ? 1 : 0;
        if (state == lockStateSent_)
            return; // only on change: no per-iteration traffic
        lockStateSent_ = state;
        const uint8_t b = uint8_t(state);
        Send(Msg::LockState, &b, 1);
    }

    void Session::SendError(ErrorCode code, const char *text)
    {
        LOGI("session error %d: %s", code, text);
        EndAs(SessionEnd::Failed);
        std::vector<uint8_t> p = ErrorPayload(code, text);
        Send(Msg::Error, p.data(), uint32_t(p.size()));
        if (status_)
            status_(L"Error: " + Widen(text));
    }
    void Session::Run(FILE *dump, int dumpSeconds)
    {
        MakeRealtimeThread(L"Capture"); // this thread captures, encodes and sends
        dump_ = dump;
        startUs_ = lastSecondUs_ = NowUs();
        lastRecvUs_ = startUs_;
        deadlineUs_ = dumpSeconds > 0 ? startUs_ + uint64_t(dumpSeconds) * 1000000 : 0;
        CancelScreenRestore(); // this session arranges the screens now; an earlier one's retry must not undo that
        bool prepared = false;
        {
            std::optional<WaitingPing> waiting; // until the receive thread and the stream take over
            if (conn_.s != INVALID_SOCKET)
                waiting.emplace(conn_, sendMutex_);
            prepared = PrepareDisplay();
        }
        if (prepared)
            Serve();
        Teardown(); // every way out, so a monitor switched on above is switched off again
    }

    // Chooses the stream format and the GPU, switches the virtual monitor on and attaches it, arranges the real screens. Slow
    // (seconds): the caller keeps the tablet waiting with pings. false = the session cannot go on (the tablet has been told).
    bool Session::PrepareDisplay()
    {
        ChooseStreamFormat();
        ChooseMonitorGpu();
        VirtualDisplay vd;
        bool switchedOn = false;
        if (!BringUpMonitor(vd, switchedOn))
            return false;
        SetVddFailed(false);
        gdiName_ = vd.gdiName;
        monitorUsed_ = true;
        // With the virtual monitor off the desktop, the real screens are exactly as the user arranged them: remember that
        // (never while it is attached: Windows shifts things then). Switching it on above has done so already, and the
        // screens may have moved since.
        if (!switchedOn && settings_.displayOverride.empty() && !vd.attached)
            RememberRealScreens();
        const DisplayMode want{hello_.screenW ? hello_.screenW : 1920, hello_.screenH ? hello_.screenH : 1080, fps_};
        if (!AddTabletModeToDriver(want, vd))
            return false;
        DisplayMode active{};
        if (!AttachMonitor(want, active))
            return false;
        StartSecureDesktopHelp();
        WatchLayoutWhileStarting();
        if (active.hz >= 10 && active.hz < fps_)
        {
            LOGI("session: tablet wants %d Hz but the virtual display offers %d Hz at this resolution", fps_, active.hz);
            fps_ = active.hz;
        }
        LOGI("session: %ls %dx%d@%d, codec %s, cap %d kbps (%s)", gdiName_.c_str(), active.width, active.height, active.hz,
             codec_ == CodecHEVC ? "HEVC" : "H.264", maxBitrateKbps_, usb_ ? "USB" : "Wi-Fi");
        TagStreamForWifi();
        if (!flowControl_ && conn_.s != INVALID_SOCKET)
            LOGI("session: client speaks protocol v%u, no flow control (update the tablet app)", hello_.version);
        return true;
    }

    void Session::ChooseStreamFormat()
    {
        // Auto: H.264 over USB (bandwidth to spare, fastest encode), HEVC over Wi-Fi (better quality per bit).
        bool wantHevc = settings_.codec == L"hevc" || (settings_.codec == L"auto" && !usb_);
        bool hevcOk = (hello_.codecMask & MaskHEVC) != 0, h264Ok = (hello_.codecMask & MaskH264) != 0;
        codec_ = (wantHevc && hevcOk) || !h264Ok ? CodecHEVC : CodecH264;
        maxBitrateKbps_ = (usb_ ? settings_.usbBitrateMbps : settings_.wifiBitrateMbps) * 1000;
        // Wi-Fi capacity is unknown: start at 60% of the cap and let adaptation ramp up.
        bitrateKbps_ = usb_ || !settings_.adaptive ? maxBitrateKbps_ : std::max(settings_.minBitrateMbps * 1000, maxBitrateKbps_ * 6 / 10);
        flowControl_ = conn_.s != INVALID_SOCKET && hello_.version >= 2;
        // The tablet asks for its panel refresh rate; the virtual monitor switches to the closest mode.
        fps_ = std::clamp(std::min(settings_.maxFps, hello_.maxFps ? int(hello_.maxFps) : 60), 10, 240);
    }

    // Finds the virtual monitor, switching it on if that is what it takes. false = there is none (the tablet has been told).
    bool Session::BringUpMonitor(VirtualDisplay &vd, bool &switchedOn)
    {
        bool switchOnFailed = false;
        if (!FindVirtualDisplay(settings_.displayOverride, vd) && settings_.displayOverride.empty() && QueryVdd() == VddState::Disabled)
        {
            // Switched off on exit or while no tablet was connected: bring it back. The scheduled task, pnputil and the driver's
            // monitor arrival can take several seconds together (more right after boot); a shorter wait ended the session while
            // the monitor was still coming. The pings above keep the tablet waiting, and both waits end as soon as it is there.
            switchedOn = SetVirtualMonitorEnabled(true, kMonitorSwitchOnMs);
            bool changed = false;
            if (!switchedOn && !fallbackGpu_.empty() && SetVddGpu(fallbackGpu_, changed))
            {
                LOGI("session: the virtual monitor did not come up on the chosen GPU, trying %ls", fallbackGpu_.c_str());
                switchedOn = SetVirtualMonitorEnabled(true, kMonitorSwitchOnMs);
            }
            switchOnFailed = !switchedOn;
            monitorUsed_ = switchedOn;
            for (int i = 0; switchedOn && i < 100 && !FindVirtualDisplay(L"", vd); ++i)
                Sleep(100);
        }
        if (!vd.gdiName.empty())
            return true;
        VddState state = settings_.displayOverride.empty() ? QueryVdd() : VddState::Ready;
        // Each text names the item the tray shows for that state; SetVddFailed makes sure it is shown.
        SetVddFailed(true);
        SendError(ErrNoDisplay,
                  state == VddState::NotInstalled ? "The virtual monitor driver is not installed on the PC. Right-click the PadDisplay tray icon and choose 'Install virtual monitor driver'."
                  : state == VddState::Disabled && switchOnFailed
                      ? "PadDisplay could not switch the virtual monitor on. Right-click the PadDisplay tray icon on the PC and choose 'Enable virtual monitor driver'."
                  : state == VddState::Disabled ? "The virtual monitor driver is disabled on the PC. Right-click the PadDisplay tray icon and choose 'Enable virtual monitor driver'."
                                                : "Virtual monitor not found on the PC. Right-click the PadDisplay tray icon and choose 'Repair virtual monitor driver'.");
        return false;
    }

    // Adds the tablet's exact resolution / refresh rate to the driver if it is missing (no admin needed).
    bool Session::AddTabletModeToDriver(const DisplayMode &want, VirtualDisplay &vd)
    {
        bool reloaded = false;
        if (!settings_.matchClientResolution || !settings_.displayOverride.empty() || !EnsureVddMode(want.width, want.height, want.hz, reloaded) || !reloaded)
            return true;
        // The driver re-plugs its monitor; wait for the new mode (the GDI name can change too).
        // Keep pinging meanwhile so the tablet does not time out.
        uint64_t waitStart = NowUs();
        for (int i = 0; i < 80; ++i)
        {
            Sleep(100);
            if (i % 5 == 0 && conn_.s != INVALID_SOCKET)
            {
                uint8_t ping[16] = {};
                wr64(ping, NowUs());
                if (!Send(Msg::Ping, ping, sizeof(ping)))
                    return false;
            }
            if (!FindVirtualDisplay(L"", vd))
                continue;
            auto modes = ListModes(vd.gdiName);
            if (std::any_of(modes.begin(), modes.end(), [&](auto &m)
                            { return m.width == want.width && m.height == want.height && m.hz == want.hz; }))
                break;
        }
        LOGI("session: driver reloaded with %dx%d@%d after %llu ms", want.width, want.height, want.hz, (NowUs() - waitStart) / 1000);
        gdiName_ = vd.gdiName;
        return true;
    }

    bool Session::AttachMonitor(const DisplayMode &want, DisplayMode &active)
    {
        VirtualPlacement saved;
        bool haveSaved = settings_.displayOverride.empty() && LoadVirtualPlacement(want.width, want.height, saved);
        if (!EnsureAttached(gdiName_, want, settings_.matchClientResolution, haveSaved ? &saved : nullptr, active))
        {
            SendError(ErrNoDisplay, "Could not attach the virtual display to the desktop.");
            return false;
        }
        havePlacement_ = CurrentPlacement(gdiName_, placement_);
        onlyScreen_ = (hello_.flags & HelloOnlyScreen) != 0 && conn_.s != INVALID_SOCKET && settings_.displayOverride.empty();
        if (onlyScreen_ && !ShowOnlyOn(gdiName_))
        {
            onlyScreen_ = false;
            SendNotice("Could not switch the PC's own screens off. The tablet is an extra screen for now. See the PadDisplay log.");
        }
        return true;
    }

    // Two secure-desktop features share one System helper (uacrelay.*), which only an elevated host can
    // register and drive:
    //  - UAC-click (only-screen + uacClickableOnTablet): move prompts off the secure desktop so the tablet
    //    can show them, and click them through the helper.
    //  - lock-screen control (lockScreenControl, any mode): see and type into the locked sign-in screen.
    void Session::StartSecureDesktopHelp()
    {
        const bool wantUac = onlyScreen_ && settings_.uacClickableOnTablet;
        const bool wantLock = settings_.lockScreenControl;
        if (!wantUac && !wantLock)
            return;
        if (!HostIsElevated())
        {
            LOGI("session: secure-desktop help needs PadDisplay elevated (run as administrator); not available");
            return;
        }
        if (wantUac)
            AllowUacClicksOnTablet(); // prompts onto the normal desktop; restored on teardown / at start-up
        std::wstring msg;
        const bool task = EnsureUacRelayTask(msg);
        uacClickable_ = task && wantUac;
        lockControl_ = task && wantLock;
        if (!task)
            LOGI("session: secure-desktop helper not available: %s", Narrow(msg).c_str());
    }

    // Attaching/reloading may have moved real screens. Windows re-applies its own layout for a
    // moment afterwards, so watching for that takes 1.5 s: do it while the stream starts.
    void Session::WatchLayoutWhileStarting()
    {
        if (!settings_.displayOverride.empty())
            return;
        layoutThread_ = std::thread([this]
                                    {
        if (!onlyScreen_) {
            if (RestoreRealScreens() > 0) layoutChanged_ = true;
            return;
        }
        for (int i = 0; i < 8 && !stop_; ++i) { // that re-applied layout switches the real screens back on
            Sleep(250);
            if (!SnapshotRealDisplays().empty() && ShowOnlyOn(gdiName_)) layoutChanged_ = true;
        } });
    }

    // Marks the stream as audio/video so Wi-Fi (WMM) gives it the video access category. Off by default: on some routers the
    // tagged host->tablet frames were dropped entirely.
    void Session::TagStreamForWifi()
    {
        if (conn_.s == INVALID_SOCKET || usb_ || !settings_.wifiQos)
            return;
        QOS_VERSION ver{1, 0};
        if (!QOSCreateHandle(&ver, &qos_) ||
            !QOSAddSocketToFlow(qos_, conn_.s, nullptr, QOSTrafficTypeAudioVideo, QOS_NON_ADAPTIVE_FLOW, reinterpret_cast<PQOS_FLOWID>(&qosFlow_)))
            LOGI("session: QoS tagging unavailable (%lu)", GetLastError());
    }

    // Streams until the tablet leaves or the session is stopped; injects the tablet's input meanwhile.
    void Session::Serve()
    {
        if (conn_.s != INVALID_SOCKET)
            StartInputThreads();
        int failures = 0;
        Told told = Told::None;
        while (!stop_)
        {
            int r = SetupPipeline();
            // A locked PC or a UAC prompt can stay up for minutes and is no failure: keep the tablet connected (it gives up
            // after a few seconds of silence) and wait.
            if (r < 0 || (r == 0 && !secureDesktop_ && ++failures > kPipelineRetries))
            {
                SendError(ErrEncoder, "Could not start capture/encoding on the PC. See the host log.");
                break;
            }
            if (r == 0)
            {
                if (!WaitOutSecureDesktop(told))
                    break;
                continue;
            }
            failures = 0;
            told = Told::None;
            SendLockState(false); // streaming the desktop normally: hide the password box
            if (!StreamLoop())
                break;
        }
    }

    void Session::StartInputThreads()
    {
        input_ = std::make_unique<InputInjector>(HostIsElevated(), [this](const char *text)
                                                 { SendNotice(text); });
        if (uacClickable_ || lockControl_)
        {
            uacClick_ = std::make_unique<UacClickClient>([this](const char *text)
                                                         { SendNotice(text); }, lockControl_);
            // Called on the input thread for every tap / move / key while a secure-desktop feature is on.
            // Each method returns at once unless its target (a UAC prompt, or the locked sign-in screen) is
            // actually up, so normal input costs only a cheap check; the worker thread does the pipe work.
            input_->SetUacForward([this](const UacForwardEvent &e)
                                  {
            switch (e.kind) {
            case UacForwardEvent::Click: uacClick_->Click(e.x, e.y); break;
            case UacForwardEvent::KeyVk: uacClick_->Key(e.vk); break;
            case UacForwardEvent::KeyScan: uacClick_->KeyScan(e.code, e.down); break;
            case UacForwardEvent::MouseMove: uacClick_->MouseMove(e.x, e.y); break;
            } });
        }
        recvThread_ = std::thread([this]
                                  { RecvLoop(); });
        inputThread_ = std::thread([this]
                                   { InputLoop(); });
    }

    // One pass while capture cannot be built. Capture is refused both when the PC is locked (the Winlogon desktop) and while a UAC
    // prompt is up: they look the same to the capture call, WM_WTSSESSION_CHANGE tells them apart. false = the session is over.
    bool Session::WaitOutSecureDesktop(Told &told)
    {
        const Told reason = SessionIsLocked() ? Told::Locked : Told::Uac;
        bool streamed = false;
        if (!StreamLockScreen(reason, streamed))
            return false;

        // Offer the tablet's password box when the picture is frozen because we cannot show the sign-in
        // screen AND we can actually type what the user enters: lock-screen control is on and the injector
        // is up (lockControl_ && uacClick_). It covers a mid-session lock (reason Locked) and the pre-sign-in
        // service, where every frozen secure desktop IS the sign-in screen (SessionIsLocked has no window
        // proc there, so reason reads as Uac). Not for a genuine mid-session UAC freeze, not while the lock
        // screen itself is streaming, and never when the box's input would be dropped for lack of the feature
        // (that silent drop was the bug: the box would appear, take a password, and discard it).
        const bool offerPasswordBox = lockControl_ && uacClick_ && !streamed &&
                                      (reason == Told::Locked || settings_.preSignInAccess);
        // The "sign in on the PC itself" notice contradicts an open password box, so send it only when no
        // box is offered; when the box is up, the tablet's own box status tells the user what to do.
        if (!streamed && secureDesktop_ && told != reason && !offerPasswordBox)
        {
            told = reason;
            if (reason == Told::Locked)
                SendNotice(onlyScreen_ ? "The PC is locked. The tablet cannot show the Windows sign-in screen, and the PC's own screens are off. Sign in on the PC, or disconnect the tablet to get the PC screens back."
                                       : "The PC is locked. Sign in on the PC's own screen; the picture continues afterwards.");
            else
                SendNotice(onlyScreen_ ? "Windows is asking for administrator approval (UAC). The tablet cannot show that prompt, and the PC's own screens are off. Answer it with the PC's keyboard, or disconnect the tablet to get the PC screens back."
                                       : "Windows is asking for administrator approval (UAC). The tablet cannot show that prompt: answer it on the PC's own screen. The picture continues afterwards.");
        }
        if (streamed)
            told = Told::None; // streaming again: a later freeze should re-notify
        SendLockState(offerPasswordBox);
        if (!Heartbeat(NowUs()))
            return false;
        if (!streamed)
            Sleep(200); // lockCap_.Next already waited up to 100 ms for a frame
        return true;
    }

    // Lock-screen capture (piece 3): if the user enabled it, the PC is locked, and we already have an encoder (locked mid-session),
    // the System helper captures the sign-in desktop into our shared texture and we stream it so the tablet can see and sign in.
    // Starting a session while already locked (no encoder yet) still just shows the notice. false = the connection is gone.
    bool Session::StreamLockScreen(Told reason, bool &streamed)
    {
        if (reason != Told::Locked || !lockControl_ || !uacClick_ || !encoder_ || !device_ || width_ <= 0)
        {
            if (lockCap_.Active())
            { // no longer locked (or feature off): stop the helper capture
                lockCap_.Stop();
                uacClick_->StopCapture();
            }
            return true;
        }
        // Capture the lock screen from a REAL display, never the tablet's own virtual monitor: when the tablet is the only
        // screen, Windows moves the sign-in screen to the real laptop screen, and an indirect-display adapter cannot share a
        // capture texture anyway. gdiName_ is that virtual monitor, so the lock capture skips it.
        if (!lockCap_.Active() && lockCap_.Start(device_.Get(), width_, height_, gdiName_))
            uacClick_->StartCapture(lockCap_.AdapterLuid(), width_, height_);
        bool fresh = false;
        if (!lockCap_.Active() || !lockCap_.Next(device_.Get(), 100, encoder_->InputTexture(), fresh))
            return true;
        bool pendingIdr = !configSent_; // first locked frame carries Config + a keyframe
        const Emit e = EmitFrame(pendingIdr);
        if (e == Emit::SendFailed)
            return false;
        if (e == Emit::EncoderFailed)
            encoder_.reset(); // SetupPipeline rebuilds it
        else
            streamed = true;
        return true;
    }

    // Ends the session's threads and puts the PC back: the controllers, the real screens and the virtual monitor.
    void Session::Teardown()
    {
        stop_ = true;
        if (lockCap_.Active())
        {
            lockCap_.Stop();
            if (uacClick_)
                uacClick_->StopCapture();
        } // end any locked-screen capture
        if (recvThread_.joinable())
            recvThread_.join();
        if (inputThread_.joinable())
            inputThread_.join();
        if (layoutThread_.joinable())
            layoutThread_.join();
        if (input_)
            input_->ReleaseAll();
        gamepads_.reset();         // unplugs the virtual controllers
        EndAs(SessionEnd::Failed); // no reason given (the test mode's time ran out): put everything back at once
        const SessionEnd why = end_;
        const bool ownMonitor = conn_.s != INVALID_SOCKET && settings_.displayOverride.empty() && monitorUsed_;
        // The windows on the virtual monitor stay where they are while it stays on: for the session that replaces this one
        // (most often this tablet again, its old link not yet timed out), or for a while for a tablet that reconnects by itself
        // after losing the link. Never in only-screen mode for a lost link: the PC's own screens must come back at once.
        const bool nextSessionFollows = why == SessionEnd::Replaced;
        keepMonitor_ = ownMonitor && settings_.monitorOnlyWhileConnected && why == SessionEnd::Lost && !onlyScreen_ &&
                       (hello_.flags & HelloReconnects) != 0 && settings_.reconnectGraceSeconds > 0;
        const bool monitorStaysOn = nextSessionFollows || keepMonitor_;
        // How the user set the virtual monitor up (they may have moved or re-sized it in Windows' display settings during the
        // session), for the next connection. Read before the monitor leaves the desktop below.
        VirtualDisplay current;
        VirtualPlacement now;
        bool havePlacement = ownMonitor && FindVirtualDisplay(L"", current) && CurrentPlacement(current.gdiName, now);
        if (havePlacement)
            gdiName_ = current.gdiName;
        if (onlyScreen_)
        {
            // As the only screen it sits at (0,0), which is not a place the user chose: keep the one it was attached at.
            havePlacement = havePlacement && havePlacement_;
            now.x = placement_.x;
            now.y = placement_.y;
            EndOnlyScreenMode(monitorStaysOn);
        }
        if (ownMonitor)
        {
            if (havePlacement)
                SaveVirtualPlacement(hello_.screenW, hello_.screenH, now);
            if (settings_.monitorOnlyWhileConnected && !monitorStaysOn)
                SwitchVirtualMonitorOff(gdiName_);
        }
        if (keepMonitor_)
            LOGI("session: ended (%s): keeping the virtual monitor for %d s for the tablet to reconnect", SessionEndName(why), settings_.reconnectGraceSeconds);
        else
            LOGI("session: ended (%s)", SessionEndName(why));
    }

    void Session::EndOnlyScreenMode(bool monitorStaysOn)
    {
        RestoreSecureDesktop(); // put UAC prompts back on the secure desktop if we moved them off
        // The PC's own screens come back first, whatever else happens below: in one step, exactly as they were, when the
        // virtual monitor is about to be switched off anyway. Otherwise, or if Windows refuses that, back to the setup
        // Windows stored. If it refuses that too (a UAC prompt is up), ShowOnAllAgain keeps trying in the background.
        const bool inOneStep = settings_.monitorOnlyWhileConnected && !monitorStaysOn && ShowRealScreensOnly();
        if (!inOneStep && ShowOnAllAgain())
            RestoreRealScreens();
    }
    const char *SessionEndName(SessionEnd why)
    {
        switch (why)
        {
        case SessionEnd::TabletLeft:
            return "the tablet disconnected";
        case SessionEnd::Lost:
            return "connection lost";
        case SessionEnd::Replaced:
            return "a new connection took over";
        case SessionEnd::HostExit:
            return "PadDisplay exits";
        case SessionEnd::HostRestart:
            return "PadDisplay restarts";
        case SessionEnd::Failed:
            return "error";
        default:
            return "?";
        }
    }

    // The first reason given is the one that counts: a lost link noticed after the tablet said goodbye is no lost link.
    void Session::EndAs(SessionEnd why)
    {
        SessionEnd none = SessionEnd::None;
        end_.compare_exchange_strong(none, why);
    }

    // Keeps the tablet's connection alive (it gives up after a few seconds of silence) and notices when
    // the tablet stopped answering. Also runs while the pipeline cannot be built, e.g. during a UAC prompt.
    bool Session::Heartbeat(uint64_t now)
    {
        if (conn_.s == INVALID_SOCKET)
            return true;
        // Signed: the receive thread may have stamped lastRecvUs_ after `now` was read.
        if (int64_t(now) - int64_t(lastRecvUs_.load()) > int64_t(kSilenceLimitUs))
        {
            LOGI("session: client stopped responding");
            EndAs(SessionEnd::Lost);
            return false;
        }
        if (now - lastPingUs_ >= kPingIntervalUs)
        {
            uint8_t ping[16];
            wr64(ping, now);
            wr32(ping + 8, lastRttUs_);
            wr32(ping + 12, uint32_t(bitrateKbps_));
            if (!Send(Msg::Ping, ping, sizeof(ping)))
                return false;
            lastPingUs_ = now;
        }
        return true;
    }

    bool Session::GamingMode() const
    {
        return hello_.version >= 3 ? (hello_.flags & HelloGamingMode) != 0 : settings_.gamingMode;
    }

    void Session::ChooseMonitorGpu()
    {
        if (!settings_.displayOverride.empty())
            return; // the user named the display: its GPU is theirs
        const std::vector<EncodeGpu> gpus = ListEncodeGpus();
        if (gpus.size() < 2)
            return; // nothing to choose from
        GpuSituation now;
        now.gaming = GamingMode();
        now.onBattery = OnBattery();
        const EncodeGpuChoice choice = ChooseEncodeGpu(gpus, settings_.gpu, now);
        const EncodeGpu &gpu = choice.gpu;
        LOGI("session: encoding on %ls (%ls): %ls", gpu.name.c_str(), EncoderName(gpu.vendorId), choice.why);
        chosenGpu_ = gpu.name;
        // If the monitor will not come up on this GPU (the driver could not use it, or it is asleep), another one is the way out.
        auto other = std::find_if(gpus.begin(), gpus.end(), [&](const EncodeGpu &g)
                                  { return g.name != gpu.name && g.integrated; });
        if (other == gpus.end())
            other = std::find_if(gpus.begin(), gpus.end(), [&](const EncodeGpu &g)
                                 { return g.name != gpu.name; });
        fallbackGpu_ = other == gpus.end() ? L"" : other->name;

        bool changed = false;
        if (!SetVddGpu(gpu.name, changed) || !changed)
            return;
        VirtualDisplay vd;
        if (!FindVirtualDisplay(L"", vd))
            return; // it is off, which is the usual case: switching it on reads the new GPU
        // It is on (the tray option "only while connected" is off, or an earlier run left it): the driver re-plugs it on the new GPU.
        // The waiting ping keeps the tablet from timing out meanwhile.
        ReloadVddDriver();
        for (int i = 0; i < 80; ++i)
        {
            Sleep(100);
            CaptureTarget t;
            if (FindVirtualDisplay(L"", vd) && (FindRenderAdapter(vd.gdiName, t) || FindOutput(vd.gdiName, t)) && t.adapterName == gpu.name)
                return;
        }
        LOGI("session: the virtual monitor did not move to %ls in time", gpu.name.c_str());
    }

} // namespace pd
