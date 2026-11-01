// PadDisplay by Nezucloud. Copyright (c) 2026 Nezucloud.
// SPDX-License-Identifier: MIT
#include "session.h"
#include "encoder/gpu_select.h"
#include "log.h"
#include "parameter_sets.h"
#include "realtime.h"
#include "text.h"

#include <algorithm>
#include <chrono>
#include <cwchar>

namespace pd
{
    namespace
    {

        constexpr UINT kVendorNvidia = 0x10DE;
        constexpr uint64_t kAdaptHoldAfterRestartUs = 2000000; // the tablet restarts its decoder and acks late: that is not congestion

    } // namespace
    // A GPU reset (TDR) or driver update removes the device: everything made on it is dead and has to be made again.
    bool Session::DeviceLost() const
    {
        if (!device_)
            return false;
        const HRESULT reason = device_->GetDeviceRemovedReason();
        if (reason == S_OK)
            return false;
        LOGI("session: the GPU device was removed (0x%08lx), starting capture and encoding again", reason);
        return true;
    }

    // 1 ready, 0 retry later, -1 fatal.
    int Session::SetupPipeline()
    {
        CaptureTarget t;
        if (!FindOutput(gdiName_, t))
            return 0; // display still settling
        const bool wgc = ChooseCaptureAdapter(t);
        if (!EnsureDevice(t, wgc))
            return -1;
        target_ = t;

        HRESULT hr = wgc ? capture_.InitWgc(device_.Get(), gdiName_, settings_.drawCursor, unsigned(std::max(1, 500 / fps_)))
                         : capture_.Init(device_.Get(), t.output.Get(), settings_.drawCursor);
        secureDesktop_ = hr == E_ACCESSDENIED;
        if (FAILED(hr))
        {
            static int logged = 0;
            if (logged++ < 5 || hr != E_ACCESSDENIED)
                LOGI("session: %s failed 0x%08lx%s", wgc ? "Windows.Graphics.Capture" : "DuplicateOutput", hr,
                     hr == DXGI_ERROR_UNSUPPORTED ? " (Desktop Duplication cannot see the monitor from this GPU: using Windows.Graphics.Capture)" : hr == E_ACCESSDENIED ? " (secure desktop / UAC prompt active)"
                                                                                                                                                                         : "");
            if (hr == DXGI_ERROR_UNSUPPORTED && !wgc)
                wgcOnly_ = true; // the next try builds it the other way
            return 0;
        }
        if (input_)
            input_->SetTarget(t.desktopRect);
        if (settings_.profile && !probe_)
        {
            D3D11_QUERY_DESC qd{D3D11_QUERY_EVENT, 0};
            device_->CreateQuery(&qd, &probe_);
        }
        if (encoder_ && capture_.Width() == width_ && capture_.Height() == height_)
            return 1;
        return StartEncoder(t.vendorId) ? 1 : -1;
    }

    // Desktop Duplication needs the adapter this process finds the monitor under to be the one that renders it. DXGI lists the monitor
    // under whichever adapter Windows' GPU preference for this program picks (always so in only-screen mode), which is not always the
    // GPU the virtual monitor was put on: every AcquireNextFrame then fails. Windows.Graphics.Capture does not care, so it takes over.
    // true = capture with it; t is then on the GPU that renders the monitor, where that is known.
    bool Session::ChooseCaptureAdapter(CaptureTarget &t)
    {
        CaptureTarget render = t;
        bool renderKnown = FindRenderAdapter(gdiName_, render);
        // Windows could not say which GPU renders the monitor: it is the one it was asked to put it on.
        if (!renderKnown && wgcOnly_ && !chosenGpu_.empty())
            renderKnown = FindAdapterByName(chosenGpu_, render);
        const bool wgc = wgcOnly_ || (renderKnown && render.adapterName != t.adapterName);
        if (wgc && renderKnown)
        {
            t.adapter = render.adapter;
            t.vendorId = render.vendorId;
            t.adapterName = render.adapterName;
        }
        return wgc;
    }

    bool Session::EnsureDevice(const CaptureTarget &t, bool wgc)
    {
        DXGI_ADAPTER_DESC1 newDesc{}, oldDesc{};
        t.adapter->GetDesc1(&newDesc);
        if (target_.adapter)
            target_.adapter->GetDesc1(&oldDesc);
        bool sameAdapter = target_.adapter && memcmp(&newDesc.AdapterLuid, &oldDesc.AdapterLuid, sizeof(LUID)) == 0;
        if (device_ && sameAdapter && !DeviceLost())
            return true;
        encoder_.reset();
        capture_ = DesktopCapture();
        if (lockCap_.Active())
        { // it shares a texture made on the old device
            lockCap_.Stop();
            if (uacClick_)
                uacClick_->StopCapture();
        }
        probe_.Reset();
        device_.Reset();
        ctx_.Reset();
        if (!CreateDevice(t.adapter.Get(), GamingMode(), device_, ctx_))
            return false;
        LOGI("session: capturing on %ls%s", t.adapterName.c_str(),
             wgc ? " with Windows.Graphics.Capture (Desktop Duplication cannot see this monitor from this process)" : "");
        if (t.vendorId != kVendorNvidia)
            LOGI("session: encoding with Media Foundation (%ls)", EncoderName(t.vendorId));
        return true;
    }

    bool Session::StartEncoder(UINT vendorId)
    {
        width_ = capture_.Width();
        height_ = capture_.Height();
        encoder_.reset();
        std::vector<Codec> codecs{codec_};
        Codec other = codec_ == CodecHEVC ? CodecH264 : CodecHEVC;
        if (hello_.codecMask & (other == CodecHEVC ? MaskHEVC : MaskH264))
            codecs.push_back(other);
        for (Codec c : codecs)
        {
            EncoderConfig cfg{width_, height_, fps_, c, bitrateKbps_, settings_.nvencPreset};
            if (vendorId == kVendorNvidia && settings_.encoder != L"mf")
            {
                encoder_ = CreateNvencEncoder();
                if (!encoder_->Init(device_.Get(), cfg))
                    encoder_.reset();
            }
            if (!encoder_)
            {
                encoder_ = CreateMfEncoder();
                if (!encoder_->Init(device_.Get(), cfg))
                    encoder_.reset();
            }
            if (encoder_)
            {
                codec_ = c;
                break;
            }
        }
        if (!encoder_)
            return false;

        uint8_t hi[12];
        wr16(hi, kProtocolVersion);
        hi[2] = codec_;
        wr16(hi + 3, uint16_t(width_));
        wr16(hi + 5, uint16_t(height_));
        hi[7] = uint8_t(fps_);
        wr32(hi + 8, uint32_t(bitrateKbps_));
        if (!Send(Msg::HostHello, hi, sizeof(hi)))
            return false;
        adaptHoldUntilUs_ = NowUs() + kAdaptHoldAfterRestartUs;
        configSent_ = false;
        keyframeRequested_ = true;
        return true;
    }

    // Encodes encoder_->InputTexture() and sends it, Config first if the decoder has not been configured.
    Session::Emit Session::EmitFrame(bool &pendingIdr)
    {
        uint64_t t0 = NowUs();
        bool key = false;
        const bool encoded = encoder_->Encode(pendingIdr, t0 - startUs_, au_, key);
        capture_.RestoreCursorArea(encoder_->InputTexture()); // the input must hold the clean desktop for the next frame
        if (!encoded)
            return Emit::EncoderFailed;
        encodeUsThisSecond_ += NowUs() - t0;
        if (key)
            pendingIdr = false;
        if (!configSent_)
        {
            auto ps = encoder_->ParameterSets();
            if (ps.empty())
                ps = ExtractParameterSets(au_, codec_);
            if (ps.empty())
            { // cannot start the decoder without them
                pendingIdr = true;
                return Emit::NeedIdr;
            }
            if (!Send(Msg::Config, ps.data(), uint32_t(ps.size())))
                return Emit::SendFailed;
            configSent_ = true;
        }
        msg_.resize(9 + au_.size());
        wr64(msg_.data(), t0 - startUs_);
        msg_[8] = key ? 1 : 0;
        memcpy(msg_.data() + 9, au_.data(), au_.size());
        if (flowControl_)
        {
            std::lock_guard lock(inflightMutex_);
            inflight_.emplace_back(t0 - startUs_, NowUs());
        }
        ++framesSent_;
        if (!Send(Msg::Frame, msg_.data(), uint32_t(msg_.size())))
            return Emit::SendFailed;
        ++framesThisSecond_;
        bytesThisSecond_ += au_.size();
        return Emit::Sent;
    }

    // A mode change loses the capture once and the next try works. Losing it again at once, for seconds, means Windows cannot capture
    // this monitor on this GPU at all: give up with a message instead of a blank tablet and a busy loop. true = build it again.
    bool Session::OnCaptureLost(uint64_t now)
    {
        if (captureLostNoFrame_++ == 0)
            captureLostSinceUs_ = now;
        if (captureLostInRow_++ == 0 || captureLostInRow_ % 200 == 0)
            LOGI("session: duplication lost (mode change / secure desktop), restarting capture (%d since the last frame)", captureLostNoFrame_);
        if (!capture_.UsesWgc() && !wgcOnly_ && captureLostNoFrame_ >= 3 && now - captureLostSinceUs_ > 300000)
        {
            LOGI("session: Desktop Duplication cannot capture this monitor from here: switching to Windows.Graphics.Capture");
            wgcOnly_ = true;
            captureLostNoFrame_ = captureLostInRow_ = 0;
            return true; // SetupPipeline builds the capture the other way (and the encoder, if the GPU changes with it)
        }
        if (captureLostNoFrame_ >= 5 && now - captureLostSinceUs_ > 8000000)
        {
            LOGI("session: capture on %ls keeps failing: giving up", target_.adapterName.c_str());
            SendError(ErrNoDisplay, "Windows cannot capture the virtual monitor on this GPU. In the PC's tray menu, open 'Encode on' and choose another GPU (or Auto), then connect again.");
            return false;
        }
        if (captureLostInRow_ > 3)
            SleepUs(50000); // not a tight loop: each try builds the capture again
        return true;
    }

    bool Session::StreamLoop()
    {
        int refines = 0;
        uint64_t lastEncodeUs = 0;
        while (!stop_)
        {
            uint64_t now = NowUs();
            if (deadlineUs_ && now >= deadlineUs_)
                return false;
            if (layoutChanged_.exchange(false))
            {
                LOGI("session: real screens were put back, refreshing the capture target");
                return true;
            }
            if (!Heartbeat(now))
                return false;
            if (now - lastSecondUs_ >= 1000000)
            {
                AdaptBitrate();
                ReportStatus(now);
                // Once a second, not per frame: after a GPU reset Windows.Graphics.Capture just stops delivering frames,
                // which looks like an idle desktop. SetupPipeline builds a new device.
                if (DeviceLost())
                    return true;
            }

            // Pace to the display rate: the cursor alone can update far faster than 60 Hz. Waiting lets
            // duplication coalesce updates, so the next acquire returns the newest state.
            uint64_t minIntervalUs = 900000 / uint64_t(fps_);
            if (lastEncodeUs && now - lastEncodeUs < minIntervalUs)
            {
                SleepUs(minIntervalUs - (now - lastEncodeUs));
                continue;
            }

            // Flow control: never queue more than a few frames in the network. Skipped updates are not
            // lost - duplication coalesces them and the next acquire returns the newest desktop.
            const uint64_t maxInFlight = uint64_t(settings_.maxFramesInFlight);
            if (flowControl_ && framesSent_ - framesAcked_ >= maxInFlight)
            {
                // Woken by the next ack; the timeout keeps the pings and checks above running.
                {
                    std::unique_lock lock(inflightMutex_);
                    ackCv_.wait_for(lock, std::chrono::milliseconds(5), [&]
                                    { return stop_ || framesSent_ - framesAcked_ < maxInFlight; });
                }
                blockedUsThisSecond_ += NowUs() - now;
                continue;
            }

            pendingIdr_ |= keyframeRequested_.exchange(false);
            // After the picture stops changing, re-encode it a couple of times so CBR can sharpen it.
            bool refine = refines > 0 && now - lastEncodeUs >= 60000;
            bool force = pendingIdr_ || refine;
            bool fresh = false;
            ID3D11Texture2D *tex = encoder_->InputTexture();
            // Short wait: a keyframe request must not sit behind an idle desktop.
            auto r = capture_.Next(force ? 0 : 20, tex, force, fresh);
            uint64_t tCaptured = NowUs();
            if (r == DesktopCapture::Lost)
                return OnCaptureLost(tCaptured);
            if (r == DesktopCapture::Failed)
            {
                Sleep(100);
                return true;
            }
            captureLostInRow_ = 0;
            if (r == DesktopCapture::Timeout)
                continue;            // nothing changed: says nothing about whether capture works
            captureLostNoFrame_ = 0; // a real frame
            if (fresh)
                refines = 2;
            else if (refine)
                --refines;

            if (probe_)
            { // diagnostics: wait for the copy + cursor draw so it is timed separately
                ctx_->End(probe_.Get());
                ctx_->Flush();
                while (ctx_->GetData(probe_.Get(), nullptr, 0, 0) == S_FALSE)
                    YieldProcessor();
                composeUsThisSecond_ += NowUs() - tCaptured;
                copiedThisSecond_ += capture_.LastCopiedFraction();
            }
            switch (EmitFrame(pendingIdr_))
            {
            case Emit::EncoderFailed:
                encoder_.reset();
                return true; // SetupPipeline builds a new one
            case Emit::SendFailed:
                return false;
            case Emit::NeedIdr:
                continue;
            case Emit::Sent:
                lastEncodeUs = now;
                break;
            }
        }
        return false;
    }
    void Session::AdaptBitrate()
    {
        uint32_t rttCount = rttCount_.exchange(0);
        uint64_t rttSum = rttSumUs_.exchange(0);
        uint32_t latCount = latencyCount_.exchange(0);
        uint64_t latSum = latencySumUs_.exchange(0);
        uint64_t blockedUs = blockedUsThisSecond_;
        lastBlockedMs_ = uint32_t(blockedUs / 1000);
        blockedUsThisSecond_ = 0;
        if (!settings_.adaptive || !encoder_)
            return;
        if (NowUs() < adaptHoldUntilUs_)
        {
            stableSeconds_ = 0;
            return;
        }

        bool congested = false, clear = false;
        uint64_t avg = 0, base = 0;
        if (flowControl_)
        {
            // Signals: time spent waiting for acks (link slower than the stream) and frame delivery time.
            // Only judge the link when there was real traffic: an idle desktop sends nothing, and treating
            // that as "clear" used to ramp the bitrate to the cap right before the next burst of motion.
            const uint64_t frameUs = 1000000 / uint64_t(fps_);
            const bool traffic = latCount >= 10;
            avg = latCount ? latSum / latCount : 0;
            if (traffic)
            {
                // Baseline = best 1 s average over the last ~10 s of traffic, so it follows the link.
                windowMinUs_ = std::min(windowMinUs_, avg);
                if (++windowSeconds_ >= 10)
                {
                    minLatencyUs_ = windowMinUs_;
                    windowMinUs_ = UINT64_MAX;
                    windowSeconds_ = 0;
                }
                else
                {
                    minLatencyUs_ = std::min(minLatencyUs_, avg);
                }
            }
            base = minLatencyUs_ == UINT64_MAX ? 0 : minLatencyUs_;
            congested = blockedUs > 100000 || (traffic && avg > base + std::max<uint64_t>(30000, 2 * frameUs));
            clear = traffic && blockedUs < 10000 && avg < base + frameUs;
        }
        else
        {
            if (rttCount == 0)
                return;
            avg = rttSum / rttCount;
            minRttUs_ = std::min(minRttUs_, avg);
            base = minRttUs_;
            congested = avg > base + 40000;
            clear = avg < base + 15000;
        }
        int next = bitrateKbps_;
        if (congested)
        {
            next = std::max(settings_.minBitrateMbps * 1000, bitrateKbps_ * 3 / 4);
            stableSeconds_ = 0;
        }
        else if (clear)
        {
            if (++stableSeconds_ >= 2 && bitrateKbps_ < maxBitrateKbps_)
            {
                next = std::min(maxBitrateKbps_, bitrateKbps_ * 115 / 100 + 500);
                stableSeconds_ = 0;
            }
        }
        else
        {
            stableSeconds_ = 0;
        }
        if (next != bitrateKbps_ && encoder_->SetBitrate(next))
        {
            LOGI("session: bitrate %d -> %d kbps (%s %llu us, base %llu us, waited %llu ms)", bitrateKbps_, next,
                 flowControl_ ? "frame latency" : "rtt", avg, base, blockedUs / 1000);
            bitrateKbps_ = next;
        }
    }

    void Session::ReportStatus(uint64_t now)
    {
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
                           std::to_wstring(int(copiedPct)) + L"%) · ")
                              .c_str()
                        : L"",
                 encMs, lastBlockedMs_,
                 conn_.s == INVALID_SOCKET ? L"test" : usb_ ? L"USB"
                                                            : L"Wi-Fi");
        if (status_)
            status_(s);
        if (++statusCount_ % 10 == 0 || !status_)
            LOGI("stats: %s", Narrow(s).c_str());
        framesThisSecond_ = 0;
        bytesThisSecond_ = 0;
        encodeUsThisSecond_ = 0;
        composeUsThisSecond_ = 0;
        copiedThisSecond_ = 0;
        lastSecondUs_ = now;
    }

} // namespace pd
