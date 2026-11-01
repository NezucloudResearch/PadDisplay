// PadDisplay by Nezucloud. Copyright (c) 2026 Nezucloud.
// SPDX-License-Identifier: MIT
#include "camsink.h"
#include "camdecoder.h"
#include "camshared.h"
#include "log.h"
#include "text.h"
#include "realtime.h"
#include "protocol.h"
#include "vcam.h"

#include <windows.h>
#include <cstring>
#include <thread>

namespace pd
{
    namespace
    {

        constexpr uint32_t kMaxCamPayload = 4 * 1024 * 1024; // an IDR at 1080p30 is a few hundred KB
        constexpr size_t kCamFrameHeader = 9;                // u64 ptsUs, u8 flags
        constexpr uint8_t kCamFrameIsKey = 1;
        constexpr uint32_t kDefaultFps = 30;
        constexpr DWORD kFinishDrainMs = 1000;
        // The delay between an app opening the webcam and the tablet's camera starting. This polling only runs while a tablet's
        // camera connection exists, never when idle.
        constexpr DWORD kWatchIntervalMs = 250;

    } // namespace

    CamSession::CamSession(Conn conn, const Settings &settings, uint16_t width, uint16_t height, uint8_t fps, StatusFn status)
        : conn_(std::move(conn)), settings_(settings), width_(width), height_(height), fps_(fps), status_(std::move(status))
    {
        stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    }

    CamSession::~CamSession()
    {
        Stop();
        if (stopEvent_)
            CloseHandle(stopEvent_);
        CloseSocket();
    }

    void CamSession::Stop()
    {
        stop_ = true;
        if (stopEvent_)
            SetEvent(stopEvent_);
        std::lock_guard lock(socketMutex_);
        if (conn_.s != INVALID_SOCKET)
            CutOff(conn_.s); // this connection has no receive timeout to fall back on
    }

    void CamSession::Run()
    {
        MakeRealtimeThread(L"Capture");
        if (const char *refusal = WhyRefused())
            SendNotice(refusal);
        else
            DecodeUntilEnded();
        LOGI("camera: ended (%llu frames in, %llu published, %llu decode errors)", framesIn_, framesOut_, decodeErrors_);
        // Every way out ends the connection cleanly, after any Notice has gone out, so the tablet stops its camera.
        FinishConnection(conn_.s, kFinishDrainMs);
        CloseSocket();
    }

    const char *CamSession::WhyRefused() const
    {
        if (!settings_.acceptTabletCamera)
            return "The camera is switched off in PadDisplay on the PC (settings.ini: acceptTabletCamera).";
        const CamState state = QueryCamera();
        if (state == CamState::Unsupported)
            return "The tablet's camera as a PC webcam needs Windows 11.";
        if (state == CamState::NotInstalled)
            return "The virtual camera is not set up on the PC. Right-click the PadDisplay tray icon and choose 'Set up the tablet's camera as a webcam'.";
        if (width_ == 0 || height_ == 0 || width_ > kCamMaxWidth || height_ > kCamMaxHeight)
            return "The camera size is not supported (up to 1920x1080).";
        return nullptr;
    }

    void CamSession::DecodeUntilEnded()
    {
        CamDecoder decoder([this](const Nv12Frame &frame)
                           {
        if (mailbox_.Publish(frame)) ++framesOut_; });
        if (!decoder.Init(width_, height_, fps_ ? fps_ : kDefaultFps))
            return SendNotice("This PC could not start an H.264 decoder for the camera.");
        LOGI("camera: tablet camera %ux%u@%u", width_, height_, fps_);

        SendState(false); // nothing is captured until a PC app opens the webcam
        std::thread watcher([this]
                            { Watch(); });
        DWORD noTimeout = 0; // the tablet sends nothing while no app has the webcam open; TCP keepalive catches a vanished tablet
        setsockopt(conn_.s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<char *>(&noTimeout), sizeof(noTimeout));
        ReceiveStream(decoder);
        stop_ = true;
        SetEvent(stopEvent_);
        watcher.join();
    }

    void CamSession::ReceiveStream(CamDecoder &decoder)
    {
        Msg type{};
        std::vector<uint8_t> payload;
        while (!stop_ && RecvMsg(conn_, type, payload, kMaxCamPayload))
        {
            if (type == Msg::CamConfig)
                OnConfig(decoder, payload);
            else if (type == Msg::CamFrame && payload.size() > kCamFrameHeader)
                OnFrame(decoder, payload);
        }
    }

    void CamSession::OnConfig(CamDecoder &decoder, const std::vector<uint8_t> &config)
    {
        decoder.Flush();
        needKey_ = true;
        if (!decoder.Decode(config.data(), config.size(), 0))
            RecoverFromDecodeError(decoder);
    }

    void CamSession::OnFrame(CamDecoder &decoder, const std::vector<uint8_t> &message)
    {
        if (!active_.load())
        {                    // nobody is watching: a late frame from before the tablet stopped
            needKey_ = true; // and the frames still behind it refer to this one
            return;
        }
        const bool key = (message[8] & kCamFrameIsKey) != 0;
        if (needKey_ && !key)
            return; // nothing to decode against yet
        needKey_ = false;
        ++framesIn_;
        const int64_t ptsUs = int64_t(rd64(message.data()));
        if (!decoder.Decode(message.data() + kCamFrameHeader, message.size() - kCamFrameHeader, ptsUs))
            RecoverFromDecodeError(decoder);
    }

    void CamSession::RecoverFromDecodeError(CamDecoder &decoder)
    {
        ++decodeErrors_;
        needKey_ = true;
        decoder.Flush();
        std::lock_guard lock(sendMutex_);
        SendMsg(conn_, Msg::CamKeyframeReq, nullptr, 0);
    }

    void CamSession::Watch()
    {
        while (WaitForSingleObject(stopEvent_, kWatchIntervalMs) == WAIT_TIMEOUT)
        {
            const bool active = mailbox_.Open() && mailbox_.ReaderIsActive();
            if (active == active_.load())
                continue;
            active_ = active;
            SendState(active);
            LOGI("camera: a PC app %s the webcam", active ? "opened" : "closed");
        }
    }

    void CamSession::SendNotice(const char *text)
    {
        {
            std::lock_guard lock(sendMutex_);
            SendMsg(conn_, Msg::Notice, text, uint32_t(strlen(text)));
        }
        if (status_)
            status_(L"Camera: " + Widen(text));
    }

    void CamSession::SendState(bool active)
    {
        const uint8_t v = active ? 1 : 0;
        std::lock_guard lock(sendMutex_);
        SendMsg(conn_, Msg::CamState, &v, 1);
    }

    void CamSession::CloseSocket()
    {
        std::lock_guard lock(socketMutex_);
        if (conn_.s != INVALID_SOCKET)
            closesocket(conn_.s);
        conn_.s = INVALID_SOCKET;
    }

} // namespace pd
