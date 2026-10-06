// PadDisplay by Nezucloud. Copyright (c) 2026 Nezucloud.
// SPDX-License-Identifier: MIT
#include "session.h"
#include "display.h"
#include "log.h"
#include "vdd.h"

#include <ws2tcpip.h>
#include <qos2.h>
#include <algorithm>
#include <chrono>
#include <cwchar>

namespace pd {

Session::Session(SOCKET sock, const ClientHello& hello, bool usb, const Settings& settings, StatusFn status)
    : sock_(sock), hello_(hello), usb_(usb), settings_(settings), status_(std::move(status)) {}

Session::~Session() {
    Stop();
    if (recvThread_.joinable()) recvThread_.join();
    if (inputThread_.joinable()) inputThread_.join();
    if (layoutThread_.joinable()) layoutThread_.join();
    gamepads_.reset(); // its rumble threads send on the socket
    if (qos_) {
        if (qosFlow_) QOSRemoveSocketFromFlow(qos_, sock_, qosFlow_, 0);
        QOSCloseHandle(qos_);
    }
    if (sock_ != INVALID_SOCKET) closesocket(sock_);
}

void Session::Stop() {
    stop_ = true;
    if (sock_ != INVALID_SOCKET) shutdown(sock_, SD_BOTH); // unblocks send/recv in the session threads
}

bool Session::Send(Msg type, const void* payload, uint32_t len) {
    if (dump_) {
        auto* p = static_cast<const uint8_t*>(payload);
        if (type == Msg::Config) fwrite(p, 1, len, dump_);
        if (type == Msg::Frame && len > 9) fwrite(p + 9, 1, len - 9, dump_);
        return true;
    }
    if (sock_ == INVALID_SOCKET) return false;
    std::lock_guard lock(sendMutex_);
    return SendMsg(sock_, type, payload, len);
}

void Session::SendNotice(const char* text) {
    Send(Msg::Notice, text, uint32_t(strlen(text)));
    if (status_) status_(L"Error: " + Widen(text));
}

void Session::SendError(ErrorCode code, const char* text) {
    LOGI("session error %d: %s", code, text);
    std::vector<uint8_t> p(1 + strlen(text));
    p[0] = code;
    memcpy(p.data() + 1, text, p.size() - 1);
    Send(Msg::Error, p.data(), uint32_t(p.size()));
    if (status_) status_(L"Error: " + Widen(text));
}

void Session::Run(FILE* dump, int dumpSeconds) {
    MakeRealtimeThread(L"Capture"); // this thread captures, encodes and sends
    dump_ = dump;
    startUs_ = lastSecondUs_ = NowUs();
    lastRecvUs_ = startUs_;
    deadlineUs_ = dumpSeconds > 0 ? startUs_ + uint64_t(dumpSeconds) * 1000000 : 0;
    CancelScreenRestore(); // this session arranges the screens now; an earlier one's retry must not undo that

    // Auto: H.264 over USB (bandwidth to spare, fastest encode), HEVC over Wi-Fi (better quality per bit).
    bool wantHevc = settings_.codec == L"hevc" || (settings_.codec == L"auto" && !usb_);
    bool hevcOk = (hello_.codecMask & MaskHEVC) != 0, h264Ok = (hello_.codecMask & MaskH264) != 0;
    codec_ = (wantHevc && hevcOk) || !h264Ok ? CodecHEVC : CodecH264;
    maxBitrateKbps_ = (usb_ ? settings_.usbBitrateMbps : settings_.wifiBitrateMbps) * 1000;
    // Wi-Fi capacity is unknown: start at 60% of the cap and let adaptation ramp up.
    bitrateKbps_ = usb_ || !settings_.adaptive ? maxBitrateKbps_ : std::max(settings_.minBitrateMbps * 1000, maxBitrateKbps_ * 6 / 10);
    flowControl_ = sock_ != INVALID_SOCKET && hello_.version >= 2;
    // The tablet asks for its panel refresh rate; the virtual monitor switches to the closest mode.
    fps_ = std::clamp(std::min(settings_.maxFps, hello_.maxFps ? int(hello_.maxFps) : 60), 10, 240);

    VirtualDisplay vd;
    if (!FindVirtualDisplay(settings_.displayOverride, vd) && settings_.displayOverride.empty() && QueryVdd() == VddState::Disabled &&
        SetVirtualMonitorEnabled(true, 5000)) {
        for (int i = 0; i < 30 && !FindVirtualDisplay(L"", vd); ++i) Sleep(100); // switched off on exit: bring it back
    }
    if (gdiName_.empty() && vd.gdiName.empty()) {
        VddState state = settings_.displayOverride.empty() ? QueryVdd() : VddState::Ready;
        SendError(ErrNoDisplay,
                  state == VddState::NotInstalled ? "The virtual monitor driver is not installed on the PC. Right-click the PadDisplay tray icon and choose 'Install virtual monitor driver'."
                  : state == VddState::Disabled   ? "The virtual monitor driver is disabled on the PC. Right-click the PadDisplay tray icon and choose 'Enable virtual monitor driver'."
                                                  : "Virtual monitor not found on the PC. Right-click the PadDisplay tray icon and choose 'Repair virtual monitor driver'.");
        return;
    }
    gdiName_ = vd.gdiName;
    DisplayMode want{hello_.screenW ? hello_.screenW : 1920, hello_.screenH ? hello_.screenH : 1080, fps_};

    // Add the tablet's exact resolution / refresh rate to the driver if it is missing (no admin needed).
    bool reloaded = false;
    if (settings_.matchClientResolution && settings_.displayOverride.empty() && EnsureVddMode(want.width, want.height, want.hz, reloaded) && reloaded) {
        // The driver re-plugs its monitor; wait for the new mode (the GDI name can change too).
        // Keep pinging meanwhile so the tablet does not time out.
        uint64_t waitStart = NowUs();
        for (int i = 0; i < 80; ++i) {
            Sleep(100);
            if (i % 5 == 0 && sock_ != INVALID_SOCKET) {
                uint8_t ping[16] = {};
                wr64(ping, NowUs());
                if (!Send(Msg::Ping, ping, sizeof(ping))) return;
            }
            if (!FindVirtualDisplay(L"", vd)) continue;
            auto modes = ListModes(vd.gdiName);
            if (std::any_of(modes.begin(), modes.end(), [&](auto& m) { return m.width == want.width && m.height == want.height && m.hz == want.hz; }))
                break;
        }
        LOGI("session: driver reloaded with %dx%d@%d after %llu ms", want.width, want.height, want.hz, (NowUs() - waitStart) / 1000);
        gdiName_ = vd.gdiName;
    }
    // With the virtual monitor off the desktop, the real screens are exactly as the user arranged
    // them: record that as the home layout (never while it is attached - Windows shifts things then).
    VirtualPlacement attachedNow;
    if (settings_.displayOverride.empty() && !CurrentPlacement(gdiName_, attachedNow)) SaveHomeLayout(SnapshotRealDisplays());
    DisplayMode active{};
    VirtualPlacement saved;
    bool haveSaved = settings_.displayOverride.empty() && LoadVirtualPlacement(want.width, want.height, saved);
    if (!EnsureAttached(gdiName_, want, settings_.matchClientResolution, haveSaved ? &saved : nullptr, active)) {
        SendError(ErrNoDisplay, "Could not attach the virtual display to the desktop.");
        return;
    }
    onlyScreen_ = (hello_.flags & HelloOnlyScreen) != 0 && sock_ != INVALID_SOCKET && settings_.displayOverride.empty();
    if (onlyScreen_ && !ShowOnlyOn(gdiName_)) {
        onlyScreen_ = false;
        SendNotice("Could not switch the PC's own screens off. The tablet is an extra screen for now. See the PadDisplay log.");
    }
    // In only-screen mode a UAC prompt on the secure desktop would be invisible and unanswerable. If the
    // user opted in (and the host is elevated), move prompts to the normal desktop so the tablet can
    // click them. Restored on teardown and at start-up; a no-op otherwise.
    if (onlyScreen_ && settings_.uacClickableOnTablet) AllowUacClicksOnTablet();
    // Attaching/reloading may have moved real screens. Windows re-applies its own layout for a
    // moment afterwards, so watching for that takes 1.5 s: do it while the stream starts.
    if (settings_.displayOverride.empty())
        layoutThread_ = std::thread([this] {
            if (!onlyScreen_) {
                if (RestoreHomeLayout() > 0) layoutChanged_ = true;
                return;
            }
            for (int i = 0; i < 8 && !stop_; ++i) { // that re-applied layout switches the real screens back on
                Sleep(250);
                if (!SnapshotRealDisplays().empty() && ShowOnlyOn(gdiName_)) layoutChanged_ = true;
            }
        });
    if (active.hz >= 10 && active.hz < fps_) {
        LOGI("session: tablet wants %d Hz but the virtual display offers %d Hz at this resolution", fps_, active.hz);
        fps_ = active.hz;
    }
    LOGI("session: %ls %dx%d@%d, codec %s, cap %d kbps (%s)", gdiName_.c_str(), active.width, active.height, active.hz,
         codec_ == CodecHEVC ? "HEVC" : "H.264", maxBitrateKbps_, usb_ ? "USB" : "Wi-Fi");

    if (sock_ != INVALID_SOCKET && !usb_ && settings_.wifiQos) {
        // Mark the stream as audio/video so Wi-Fi (WMM) gives it the video access category. Off by
        // default: on some routers the tagged host->tablet frames were dropped entirely.
        QOS_VERSION ver{1, 0};
        if (!QOSCreateHandle(&ver, &qos_) ||
            !QOSAddSocketToFlow(qos_, sock_, nullptr, QOSTrafficTypeAudioVideo, QOS_NON_ADAPTIVE_FLOW, reinterpret_cast<PQOS_FLOWID>(&qosFlow_)))
            LOGI("session: QoS tagging unavailable (%lu)", GetLastError());
    }
    if (!flowControl_ && sock_ != INVALID_SOCKET)
        LOGI("session: client speaks protocol v%u, no flow control (update the tablet app)", hello_.version);

    if (sock_ != INVALID_SOCKET) {
        input_ = std::make_unique<InputInjector>();
        recvThread_ = std::thread([this] { RecvLoop(); });
        inputThread_ = std::thread([this] { InputLoop(); });
    }

    int failures = 0;
    bool toldAboutPrompt = false;
    while (!stop_) {
        int r = SetupPipeline();
        // ~30 s of retries (mode switch). A UAC prompt can stay up for minutes and is no failure: keep
        // the tablet connected (it gives up after a few seconds of silence) and wait for the answer.
        if (r < 0 || (r == 0 && !secureDesktop_ && ++failures > 150)) {
            SendError(ErrEncoder, "Could not start capture/encoding on the PC. See the host log.");
            break;
        }
        if (r == 0) {
            if (secureDesktop_ && !toldAboutPrompt) {
                toldAboutPrompt = true;
                SendNotice(onlyScreen_ ? "Windows is asking for administrator approval (UAC). The tablet cannot show that prompt, and the PC's own screens are off. Answer it with the PC's keyboard, or disconnect the tablet to get the PC screens back."
                                       : "Windows is asking for administrator approval (UAC). The tablet cannot show that prompt: answer it on the PC's own screen. The picture continues afterwards.");
            }
            if (!Heartbeat(NowUs())) break;
            Sleep(200);
            continue;
        }
        failures = 0;
        toldAboutPrompt = false;
        if (!StreamLoop()) break;
    }
    stop_ = true;
    if (recvThread_.joinable()) recvThread_.join();
    if (inputThread_.joinable()) inputThread_.join();
    if (layoutThread_.joinable()) layoutThread_.join();
    if (input_) input_->ReleaseAll();
    gamepads_.reset(); // unplugs the virtual controllers
    if (onlyScreen_) {
        RestoreSecureDesktop(); // put UAC prompts back on the secure desktop if we moved them off
        // The PC's own screens come back first, whatever else happens below. If Windows refuses (a UAC
        // prompt is up), ShowOnAllAgain keeps trying in the background and restores the layout itself.
        if (ShowOnAllAgain()) RestoreHomeLayout();
    }
    if (sock_ != INVALID_SOCKET && settings_.displayOverride.empty()) {
        // Remember how the user set things up (they may have moved or re-sized the virtual monitor
        // in Windows' display settings during the session) so the next connection restores it.
        VirtualDisplay current;
        VirtualPlacement now;
        if (FindVirtualDisplay(L"", current) && CurrentPlacement(current.gdiName, now)) {
            SaveVirtualPlacement(hello_.screenW, hello_.screenH, now);
            gdiName_ = current.gdiName;
        }
        if (settings_.detachOnDisconnect) {
            Detach(gdiName_);
            RestoreHomeLayout(); // detaching can make Windows shuffle the real screens
        }
    }
    LOGI("session: ended");
}

// Keeps the tablet's connection alive (it gives up after a few seconds of silence) and notices when
// the tablet stopped answering. Also runs while the pipeline cannot be built, e.g. during a UAC prompt.
bool Session::Heartbeat(uint64_t now) {
    if (sock_ == INVALID_SOCKET) return true;
    // Signed: the receive thread may have stamped lastRecvUs_ after `now` was read.
    if (int64_t(now) - int64_t(lastRecvUs_.load()) > 10000000) {
        LOGI("session: client stopped responding");
        return false;
    }
    if (now - lastPingUs_ >= 500000) {
        uint8_t ping[16];
        wr64(ping, now);
        wr32(ping + 8, lastRttUs_);
        wr32(ping + 12, uint32_t(bitrateKbps_));
        if (!Send(Msg::Ping, ping, sizeof(ping))) return false;
        lastPingUs_ = now;
    }
    return true;
}

int Session::SetupPipeline() {
    CaptureTarget t;
    if (!FindOutput(gdiName_, t)) return 0; // display still settling
    DXGI_ADAPTER_DESC1 newDesc{}, oldDesc{};
    t.adapter->GetDesc1(&newDesc);
    if (target_.adapter) target_.adapter->GetDesc1(&oldDesc);
    bool sameAdapter = target_.adapter && memcmp(&newDesc.AdapterLuid, &oldDesc.AdapterLuid, sizeof(LUID)) == 0;
    if (!device_ || !sameAdapter) {
        encoder_.reset();
        capture_ = DesktopCapture();
        device_.Reset();
        ctx_.Reset();
        // The tablet app decides; an app from before v3 cannot, so settings.ini does.
        bool gamingMode = hello_.version >= 3 ? (hello_.flags & HelloGamingMode) != 0 : settings_.gamingMode;
        if (!CreateDevice(t.adapter.Get(), gamingMode, device_, ctx_)) return -1;
        LOGI("session: capturing on %ls", t.adapterName.c_str());
        if (t.vendorId != 0x10DE)
            LOGI("session: the virtual display is not rendered by the NVIDIA GPU; using Media Foundation. "
                 "Set <gpu><friendlyname> in C:\\VirtualDisplayDriver\\vdd_settings.xml to the NVIDIA GPU for NVENC.");
    }
    target_ = t;

    HRESULT hr = capture_.Init(device_.Get(), t.output.Get(), settings_.drawCursor);
    secureDesktop_ = hr == E_ACCESSDENIED;
    if (FAILED(hr)) {
        static int logged = 0;
        if (logged++ < 5 || hr != E_ACCESSDENIED) LOGI("session: DuplicateOutput failed 0x%08lx%s", hr,
            hr == DXGI_ERROR_UNSUPPORTED ? " (hybrid GPU: restart PadDisplay so the high-performance GPU preference applies)" :
            hr == E_ACCESSDENIED ? " (secure desktop / UAC prompt active)" : "");
        return 0;
    }
    if (input_) input_->SetTarget(t.desktopRect);
    if (settings_.profile && !probe_) {
        D3D11_QUERY_DESC qd{D3D11_QUERY_EVENT, 0};
        device_->CreateQuery(&qd, &probe_);
    }

    if (!encoder_ || capture_.Width() != width_ || capture_.Height() != height_) {
        width_ = capture_.Width();
        height_ = capture_.Height();
        encoder_.reset();
        std::vector<Codec> codecs{codec_};
        Codec other = codec_ == CodecHEVC ? CodecH264 : CodecHEVC;
        if (hello_.codecMask & (other == CodecHEVC ? MaskHEVC : MaskH264)) codecs.push_back(other);
        for (Codec c : codecs) {
            EncoderConfig cfg{width_, height_, fps_, c, bitrateKbps_, settings_.nvencPreset};
            if (t.vendorId == 0x10DE && settings_.encoder != L"mf") {
                encoder_ = CreateNvencEncoder();
                if (!encoder_->Init(device_.Get(), cfg)) encoder_.reset();
            }
            if (!encoder_) {
                encoder_ = CreateMfEncoder();
                if (!encoder_->Init(device_.Get(), cfg)) encoder_.reset();
            }
            if (encoder_) {
                codec_ = c;
                break;
            }
        }
        if (!encoder_) return -1;

        uint8_t hi[12];
        wr16(hi, kProtocolVersion);
        hi[2] = codec_;
        wr16(hi + 3, uint16_t(width_));
        wr16(hi + 5, uint16_t(height_));
        hi[7] = uint8_t(fps_);
        wr32(hi + 8, uint32_t(bitrateKbps_));
        if (!Send(Msg::HostHello, hi, sizeof(hi))) return -1;
        // The tablet restarts its decoder now and acks late for a moment: that is not congestion.
        adaptHoldUntilUs_ = NowUs() + 2000000;
        configSent_ = false;
        keyframeRequested_ = true;
    }
    return 1;
}

bool Session::StreamLoop() {
    int refines = 0;
    bool pendingIdr = false;
    uint64_t lastEncodeUs = 0;
    std::vector<uint8_t> au, msg;
    while (!stop_) {
        uint64_t now = NowUs();
        if (deadlineUs_ && now >= deadlineUs_) return false;
        if (layoutChanged_.exchange(false)) {
            LOGI("session: real screens were put back, refreshing the capture target");
            return true;
        }
        if (!Heartbeat(now)) return false;
        if (now - lastSecondUs_ >= 1000000) {
            AdaptBitrate();
            ReportStatus(now);
        }

        // Pace to the display rate: the cursor alone can update far faster than 60 Hz. Waiting lets
        // duplication coalesce updates, so the next acquire returns the newest state.
        uint64_t minIntervalUs = 900000 / uint64_t(fps_);
        if (lastEncodeUs && now - lastEncodeUs < minIntervalUs) {
            SleepUs(minIntervalUs - (now - lastEncodeUs));
            continue;
        }

        // Flow control: never queue more than a few frames in the network. Skipped updates are not
        // lost - duplication coalesces them and the next acquire returns the newest desktop.
        const uint64_t maxInFlight = uint64_t(settings_.maxFramesInFlight);
        if (flowControl_ && framesSent_ - framesAcked_ >= maxInFlight) {
            // Woken by the next ack; the timeout keeps the pings and checks above running.
            {
                std::unique_lock lock(inflightMutex_);
                ackCv_.wait_for(lock, std::chrono::milliseconds(5), [&] { return stop_ || framesSent_ - framesAcked_ < maxInFlight; });
            }
            blockedUsThisSecond_ += NowUs() - now;
            continue;
        }

        pendingIdr |= keyframeRequested_.exchange(false);
        // After the picture stops changing, re-encode it a couple of times so CBR can sharpen it.
        bool refine = refines > 0 && now - lastEncodeUs >= 60000;
        bool force = pendingIdr || refine;
        bool fresh = false;
        ID3D11Texture2D* tex = encoder_->InputTexture();
        // Short wait: a keyframe request must not sit behind an idle desktop.
        auto r = capture_.Next(force ? 0 : 20, tex, force, fresh);
        uint64_t tCaptured = NowUs();
        if (r == DesktopCapture::Lost) {
            LOGI("session: duplication lost (mode change / secure desktop), restarting capture");
            return true;
        }
        if (r == DesktopCapture::Failed) {
            Sleep(100);
            return true;
        }
        if (r == DesktopCapture::Timeout) continue;
        if (fresh) refines = 2;
        else if (refine) --refines;

        if (probe_) { // diagnostics: wait for the copy + cursor draw so it is timed separately
            ctx_->End(probe_.Get());
            ctx_->Flush();
            while (ctx_->GetData(probe_.Get(), nullptr, 0, 0) == S_FALSE) YieldProcessor();
            composeUsThisSecond_ += NowUs() - tCaptured;
            copiedThisSecond_ += capture_.LastCopiedFraction();
        }
        uint64_t t0 = NowUs();
        bool key = false;
        bool encoded = encoder_->Encode(pendingIdr, t0 - startUs_, au, key);
        capture_.RestoreCursorArea(tex); // the input must hold the clean desktop for the next frame
        if (!encoded) {
            encoder_.reset(); // SetupPipeline builds a new one
            return true;
        }
        encodeUsThisSecond_ += NowUs() - t0;
        if (key) pendingIdr = false;
        if (!configSent_) {
            auto ps = encoder_->ParameterSets();
            if (ps.empty()) ps = ExtractParameterSets(au, codec_);
            if (ps.empty()) {
                pendingIdr = true; // cannot start the decoder without parameter sets
                continue;
            }
            if (!Send(Msg::Config, ps.data(), uint32_t(ps.size()))) return false;
            configSent_ = true;
        }
        msg.resize(9 + au.size());
        wr64(msg.data(), t0 - startUs_);
        msg[8] = key ? 1 : 0;
        memcpy(msg.data() + 9, au.data(), au.size());
        if (flowControl_) {
            std::lock_guard lock(inflightMutex_);
            inflight_.emplace_back(t0 - startUs_, NowUs());
        }
        ++framesSent_;
        if (!Send(Msg::Frame, msg.data(), uint32_t(msg.size()))) return false;
        lastEncodeUs = now;
        ++framesThisSecond_;
        bytesThisSecond_ += au.size();
    }
    return false;
}

void Session::RecvLoop() {
    MakeRealtimeThread(L"Games"); // ack and input latency
    std::vector<uint8_t> buf;
    Msg type{};
    while (!stop_) {
        fd_set rs;
        FD_ZERO(&rs);
        FD_SET(sock_, &rs);
        timeval tv{0, 50000};
        int n = select(0, &rs, nullptr, nullptr, &tv);
        if (n < 0) break;
        if (n > 0) {
            if (!RecvMsg(sock_, type, buf, kMaxClientPayload)) break;
            uint64_t now = NowUs();
            lastRecvUs_ = now;
            const uint8_t* p = buf.data();
            switch (type) {
            case Msg::Touch:
            case Msg::Pen:
            case Msg::Mouse:
            case Msg::MouseRel:
            case Msg::Key:
            case Msg::Gamepad: {
                {
                    std::lock_guard lock(inputMutex_);
                    inputQueue_.emplace_back(type, std::move(buf));
                }
                buf.clear(); // moved from
                inputCv_.notify_one();
                break;
            }
            case Msg::KeyframeReq: keyframeRequested_ = true; break;
            case Msg::FrameAck:
                if (buf.size() >= 8) {
                    uint64_t pts = rd64(p);
                    {
                        std::lock_guard lock(inflightMutex_);
                        while (!inflight_.empty() && inflight_.front().first < pts) inflight_.pop_front();
                        if (!inflight_.empty() && inflight_.front().first == pts) {
                            uint32_t lat = uint32_t(std::min<uint64_t>(now - inflight_.front().second, UINT32_MAX));
                            inflight_.pop_front();
                            lastLatencyUs_ = lat;
                            latencySumUs_ += lat;
                            ++latencyCount_;
                        }
                        ++framesAcked_;
                    }
                    ackCv_.notify_one(); // the capture thread may be waiting for room to send
                }
                break;
            case Msg::Pong:
                if (buf.size() >= 8 && rd64(p) <= now) {
                    uint32_t rtt = uint32_t(std::min<uint64_t>(now - rd64(p), UINT32_MAX));
                    lastRttUs_ = rtt;
                    rttSumUs_ += rtt;
                    ++rttCount_;
                }
                break;
            default: break;
            }
        }
    }
    LOGI("session: connection closed");
    stop_ = true;
    ackCv_.notify_all();
    inputCv_.notify_all();
}

void Session::InputLoop() {
    MakeRealtimeThread(L"Games"); // input injection latency
    std::deque<std::pair<Msg, std::vector<uint8_t>>> batch;
    while (!stop_) {
        {
            std::unique_lock lock(inputMutex_);
            inputCv_.wait_for(lock, std::chrono::milliseconds(50), [&] { return stop_ || !inputQueue_.empty(); });
            batch.swap(inputQueue_);
        }
        for (auto& [type, buf] : batch) {
            switch (type) {
            case Msg::Touch: input_->OnTouch(buf.data(), buf.size()); break;
            case Msg::Pen: input_->OnPen(buf.data(), buf.size()); break;
            case Msg::Mouse: input_->OnMouse(buf.data(), buf.size()); break;
            case Msg::MouseRel: input_->OnMouseRel(buf.data(), buf.size()); break;
            case Msg::Key: input_->OnKey(buf.data(), buf.size()); break;
            case Msg::Gamepad:
                if (!gamepads_)
                    gamepads_ = std::make_unique<GamepadBridge>(
                        [this](uint8_t index, uint8_t largeMotor, uint8_t smallMotor) {
                            const uint8_t rumble[3] = {index, largeMotor, smallMotor};
                            Send(Msg::Rumble, rumble, sizeof(rumble));
                        },
                        [this](const char* text) { SendNotice(text); });
                gamepads_->OnState(buf.data(), buf.size());
                break;
            default: break;
            }
        }
        batch.clear();
        input_->Tick();
    }
}

void Session::AdaptBitrate() {
    uint32_t rttCount = rttCount_.exchange(0);
    uint64_t rttSum = rttSumUs_.exchange(0);
    uint32_t latCount = latencyCount_.exchange(0);
    uint64_t latSum = latencySumUs_.exchange(0);
    uint64_t blockedUs = blockedUsThisSecond_;
    lastBlockedMs_ = uint32_t(blockedUs / 1000);
    blockedUsThisSecond_ = 0;
    if (!settings_.adaptive || !encoder_) return;
    if (NowUs() < adaptHoldUntilUs_) {
        stableSeconds_ = 0;
        return;
    }

    bool congested = false, clear = false;
    uint64_t avg = 0, base = 0;
    if (flowControl_) {
        // Signals: time spent waiting for acks (link slower than the stream) and frame delivery time.
        // Only judge the link when there was real traffic: an idle desktop sends nothing, and treating
        // that as "clear" used to ramp the bitrate to the cap right before the next burst of motion.
        const uint64_t frameUs = 1000000 / uint64_t(fps_);
        const bool traffic = latCount >= 10;
        avg = latCount ? latSum / latCount : 0;
        if (traffic) {
            // Baseline = best 1 s average over the last ~10 s of traffic, so it follows the link.
            windowMinUs_ = std::min(windowMinUs_, avg);
            if (++windowSeconds_ >= 10) {
                minLatencyUs_ = windowMinUs_;
                windowMinUs_ = UINT64_MAX;
                windowSeconds_ = 0;
            } else {
                minLatencyUs_ = std::min(minLatencyUs_, avg);
            }
        }
        base = minLatencyUs_ == UINT64_MAX ? 0 : minLatencyUs_;
        congested = blockedUs > 100000 || (traffic && avg > base + std::max<uint64_t>(30000, 2 * frameUs));
        clear = traffic && blockedUs < 10000 && avg < base + frameUs;
    } else {
        if (rttCount == 0) return;
        avg = rttSum / rttCount;
        minRttUs_ = std::min(minRttUs_, avg);
        base = minRttUs_;
        congested = avg > base + 40000;
        clear = avg < base + 15000;
    }
    int next = bitrateKbps_;
    if (congested) {
        next = std::max(settings_.minBitrateMbps * 1000, bitrateKbps_ * 3 / 4);
        stableSeconds_ = 0;
    } else if (clear) {
        if (++stableSeconds_ >= 2 && bitrateKbps_ < maxBitrateKbps_) {
            next = std::min(maxBitrateKbps_, bitrateKbps_ * 115 / 100 + 500);
            stableSeconds_ = 0;
        }
    } else {
        stableSeconds_ = 0;
    }
    if (next != bitrateKbps_ && encoder_->SetBitrate(next)) {
        LOGI("session: bitrate %d -> %d kbps (%s %llu us, base %llu us, waited %llu ms)", bitrateKbps_, next,
             flowControl_ ? "frame latency" : "rtt", avg, base, blockedUs / 1000);
        bitrateKbps_ = next;
    }
}

void Session::ReportStatus(uint64_t now) {
    double secs = double(now - lastSecondUs_) / 1e6;
    double mbps = bytesThisSecond_ * 8 / 1e6 / secs;
    double encMs = framesThisSecond_ ? encodeUsThisSecond_ / 1000.0 / framesThisSecond_ : 0;
    double gpuMs = framesThisSecond_ ? composeUsThisSecond_ / 1000.0 / framesThisSecond_ : 0;
    double copiedPct = framesThisSecond_ ? copiedThisSecond_ * 100.0 / framesThisSecond_ : 0;
    wchar_t s[384];
    swprintf(s, 384, L"%hs · %dx%d %ls · %.0f fps · %.1f/%d Mbps · RTT %.1f ms · frame %.1f ms · %lsenc %.1f ms · waited %u ms · %ls",
             hello_.name, width_, height_, codec_ == CodecHEVC ? L"HEVC" : L"H.264", framesThisSecond_ / secs, mbps,
             bitrateKbps_ / 1000, lastRttUs_ / 1000.0, lastLatencyUs_ / 1000.0,
             probe_ ? (L"gpu " + std::to_wstring(int(gpuMs * 10) / 10.0).substr(0, 4) + L" ms (copied " +
                       std::to_wstring(int(copiedPct)) + L"%) · ").c_str() : L"", encMs, lastBlockedMs_,
             sock_ == INVALID_SOCKET ? L"test" : usb_ ? L"USB" : L"Wi-Fi");
    if (status_) status_(s);
    if (++statusCount_ % 10 == 0 || !status_) LOGI("stats: %s", Narrow(s).c_str());
    framesThisSecond_ = 0;
    bytesThisSecond_ = 0;
    encodeUsThisSecond_ = 0;
    composeUsThisSecond_ = 0;
    copiedThisSecond_ = 0;
    lastSecondUs_ = now;
}

// ---------------------------------------------------------------------------------------------

std::vector<uint8_t> ExtractParameterSets(const std::vector<uint8_t>& au, Codec codec) {
    std::vector<uint8_t> out;
    const size_t n = au.size();
    auto findStart = [&](size_t from, size_t& scLen) {
        for (size_t k = from; k + 3 <= n; ++k) {
            if (au[k] || au[k + 1]) continue;
            if (au[k + 2] == 1) return scLen = 3, k;
            if (k + 3 < n && au[k + 2] == 0 && au[k + 3] == 1) return scLen = 4, k;
        }
        return scLen = 0, n;
    };
    size_t sc = 0, pos = findStart(0, sc);
    while (pos < n) {
        size_t start = pos + sc, nextSc = 0, next = findStart(start, nextSc);
        if (start < n) {
            uint8_t h = au[start];
            int t = codec == CodecHEVC ? (h >> 1) & 0x3F : h & 0x1F;
            bool isPs = codec == CodecHEVC ? (t >= 32 && t <= 34) : (t == 7 || t == 8);
            if (isPs) {
                out.insert(out.end(), {0, 0, 0, 1});
                out.insert(out.end(), au.begin() + start, au.begin() + next);
            }
        }
        pos = next;
        sc = nextSc;
    }
    return out;
}

// ---------------------------------------------------------------------------------------------

bool Server::Start() {
    listen_ = socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP);
    if (listen_ == INVALID_SOCKET) return false;
    NoInherit(listen_);
    DWORD off = 0;
    setsockopt(listen_, IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<char*>(&off), sizeof(off));
    sockaddr_in6 a{};
    a.sin6_family = AF_INET6;
    a.sin6_port = htons(kTcpPort);
    a.sin6_addr = in6addr_any;
    if (bind(listen_, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0 || listen(listen_, 4) != 0) {
        LOGI("server: cannot listen on port %u (%d) - is another PadDisplay running?", kTcpPort, WSAGetLastError());
        closesocket(listen_);
        listen_ = INVALID_SOCKET;
        return false;
    }
    LOGI("server: listening on port %u", kTcpPort);
    acceptThread_ = std::thread([this] { AcceptLoop(); });
    return true;
}

void Server::Stop() {
    stopping_ = true;
    if (acceptThread_.joinable()) acceptThread_.join(); // it polls stopping_: no need to wake it by closing the socket
    if (listen_ != INVALID_SOCKET) closesocket(listen_), listen_ = INVALID_SOCKET;
    for (int i = 0; i < 100 && handshakes_ > 0; ++i) Sleep(100); // they time out after 5 s at most
    std::lock_guard lock(sessionMutex_);
    StopSession();
    StopAudio();
}

void Server::StopAudio() {
    {
        std::lock_guard lock(mutex_);
        if (audio_) audio_->Stop();
    }
    if (audioThread_.joinable()) audioThread_.join();
    std::lock_guard lock(mutex_);
    audio_.reset();
}

void Server::UpdateSettings(const Settings& s) {
    std::lock_guard lock(mutex_);
    settings_ = s;
}

void Server::AcceptLoop() {
    while (!stopping_) {
        // Wait with a timeout rather than blocking in accept(): closing a socket does not reliably wake
        // a thread blocked in accept(), and Exit has to be able to stop this loop.
        fd_set rs;
        FD_ZERO(&rs);
        FD_SET(listen_, &rs);
        timeval tv{0, 200000};
        int ready = select(0, &rs, nullptr, nullptr, &tv);
        if (ready <= 0) {
            if (ready < 0) Sleep(100);
            continue;
        }
        sockaddr_storage addr{};
        int len = sizeof(addr);
        SOCKET s = accept(listen_, reinterpret_cast<sockaddr*>(&addr), &len);
        if (s == INVALID_SOCKET) {
            if (stopping_) break;
            Sleep(100);
            continue;
        }
        NoInherit(s);
        // Each hello/PIN exchange runs on its own thread, so a client that connects and stays silent
        // cannot block others (it times out after 5 s). Cap the number in progress.
        if (handshakes_ >= 4) {
            closesocket(s);
            continue;
        }
        ++handshakes_;
        std::thread([this, s, addr] {
            Handle(s, addr);
            --handshakes_;
        }).detach();
    }
}

ULONGLONG Server::LockedFor(const std::string& key, ULONGLONG now) {
    auto it = pinBuckets_.find(key);
    return it != pinBuckets_.end() && it->second.lockedUntil > now ? it->second.lockedUntil - now : 0;
}

void Server::RecordFailure(const std::string& key, ULONGLONG now, size_t limit) {
    auto& b = pinBuckets_[key];
    b.failures.push_back(now);
    while (!b.failures.empty() && now - b.failures.front() > 60000) b.failures.pop_front();
    if (b.failures.size() >= limit) {
        b.lockedUntil = now + 60000;
        b.failures.clear();
        LOGI("server: too many wrong PINs (%s) - locked for 60 s", key.c_str());
    }
}

void Server::StopSession() {
    {
        std::lock_guard lock(mutex_);
        if (session_) session_->Stop();
    }
    if (sessionThread_.joinable()) sessionThread_.join();
    std::lock_guard lock(mutex_);
    session_.reset();
}

bool Server::CheckPin(SOCKET s, const sockaddr_storage& addr, bool usb, uint32_t pin, const Settings& cfg) {
    auto reject = [&](ErrorCode code, const char* text) {
        std::vector<uint8_t> e(1 + strlen(text));
        e[0] = code;
        memcpy(e.data() + 1, text, e.size() - 1);
        SendMsg(s, Msg::Error, e.data(), uint32_t(e.size()));
        closesocket(s);
        return false;
    };
    if (!usb || cfg.requireUsbPin) {
        // Rate limit wrong PINs: 5/min per source, 20/min across all network sources; USB is its own bucket.
        std::string source = usb ? "usb" : AddrToString(addr);
        ULONGLONG now = GetTickCount64(), wait = 0;
        {
            std::lock_guard lock(pinMutex_);
            wait = std::max(LockedFor(source, now), usb ? 0 : LockedFor("*", now));
        }
        if (wait > 0) {
            std::string text = "Too many wrong PINs. Try again in " + std::to_string((wait + 999) / 1000) + " s.";
            return reject(ErrLockedOut, text.c_str());
        }
        if (pin != cfg.pin) {
            {
                std::lock_guard lock(pinMutex_);
                RecordFailure(source, now, 5);
                if (!usb) RecordFailure("*", now, 20);
            }
            LOGI("server: %s rejected (wrong PIN)", source.c_str());
            if (status_) status_(Widen("Error: wrong PIN from " + source + " (someone may be guessing)"));
            Sleep(1000); // slows guessing; this thread only serves this one connection
            return reject(ErrBadPin, "Wrong PIN. Right-click the PadDisplay icon in the PC's tray to see it.");
        }
    }
    return true;
}

void Server::Handle(SOCKET s, const sockaddr_storage& addr) {
    TuneSocket(s, true);
    DWORD timeout = 5000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<char*>(&timeout), sizeof(timeout));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<char*>(&timeout), sizeof(timeout));

    Msg type{};
    std::vector<uint8_t> p;
    if (!RecvMsg(s, type, p, 1024)) {
        closesocket(s);
        return;
    }
    if (type == Msg::AudioHello && p.size() >= 7) {
        Settings cfg;
        {
            std::lock_guard lock(mutex_);
            cfg = settings_;
        }
        bool usb = IsLoopback(addr);
        if (!CheckPin(s, addr, usb, rd32(&p[2]), cfg)) return;
        bool mutePc = (p[6] & AudioMutePc) != 0;
        LOGI("server: audio connection from %s%s", usb ? "USB" : AddrToString(addr).c_str(), mutePc ? " (mute PC)" : "");
        std::lock_guard replace(sessionMutex_);
        if (stopping_) {
            closesocket(s);
            return;
        }
        StopAudio(); // newest wins
        std::lock_guard lock(mutex_);
        audio_ = std::make_unique<AudioSession>(s, mutePc);
        AudioSession* audio = audio_.get();
        audioThread_ = std::thread([audio] { audio->Run(); });
        return;
    }
    if (type != Msg::ClientHello || p.size() < 16) {
        closesocket(s);
        return;
    }
    ClientHello h;
    h.version = rd16(&p[0]);
    h.screenW = rd16(&p[2]);
    h.screenH = rd16(&p[4]);
    h.dpi = rd16(&p[6]);
    h.codecMask = p[8];
    h.maxTouch = p[9];
    h.pin = rd32(&p[10]);
    h.maxFps = rd16(&p[14]);
    size_t nameAt = 16;
    if (h.version >= 3 && p.size() > 16) h.flags = p[nameAt++];
    memcpy(h.name, p.data() + nameAt, std::min<size_t>(p.size() - nameAt, sizeof(h.name) - 1));

    Settings cfg;
    {
        std::lock_guard lock(mutex_);
        cfg = settings_;
    }
    bool usb = IsLoopback(addr);
    auto reject = [&](ErrorCode code, const char* text) {
        std::vector<uint8_t> e(1 + strlen(text));
        e[0] = code;
        memcpy(e.data() + 1, text, e.size() - 1);
        SendMsg(s, Msg::Error, e.data(), uint32_t(e.size()));
        closesocket(s);
    };
    if (h.version < kMinProtocolVersion || h.version > kProtocolVersion) return reject(ErrVersion, "App and PC versions do not match. Update both.");
    if (!CheckPin(s, addr, usb, h.pin, cfg)) return;
    DWORD zero = 0;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<char*>(&zero), sizeof(zero));
    LOGI("server: client '%s' from %s (%s) screen %ux%u@%u dpi %u codecs %u%s%s", h.name, AddrToString(addr).c_str(),
         usb ? "USB" : "Wi-Fi", h.screenW, h.screenH, h.maxFps, h.dpi, h.codecMask, h.flags & HelloGamingMode ? ", gaming mode" : "",
         h.flags & HelloOnlyScreen ? ", only screen" : "");

    std::lock_guard replace(sessionMutex_);
    if (stopping_) {
        closesocket(s);
        return;
    }
    StopSession(); // newest tablet wins
    std::lock_guard lock(mutex_);
    session_ = std::make_unique<Session>(s, h, usb, cfg, status_);
    Session* session = session_.get();
    sessionThread_ = std::thread([this, session] {
        session->Run();
        if (!stopping_ && status_) status_(L"Waiting for tablet");
    });
}

} // namespace pd
