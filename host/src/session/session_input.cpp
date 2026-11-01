// PadDisplay by Nezucloud. Copyright (c) 2026 Nezucloud.
// SPDX-License-Identifier: MIT
#include "session.h"
#include "log.h"
#include "realtime.h"
#include "text.h"

#include <algorithm>
#include <chrono>
#include <string_view>

namespace pd
{
    namespace
    {

        constexpr size_t kMaxPasswordBytes = 256; // a legitimate password is short, and this bounds the injector's work

    } // namespace

    void Session::RecvLoop()
    {
        MakeRealtimeThread(L"Games"); // ack and input latency
        std::vector<uint8_t> buf;
        Msg type{};
        while (!stop_)
        {
            int n = WaitReadable(conn_, 50000);
            if (n < 0)
                break;
            if (n == 0)
                continue;
            if (!RecvMsg(conn_, type, buf, kMaxClientPayload))
                break;
            uint64_t now = NowUs();
            lastRecvUs_ = now;
            switch (type)
            {
            case Msg::Touch:
            case Msg::Pen:
            case Msg::Mouse:
            case Msg::MouseRel:
            case Msg::Key:
            case Msg::Gamepad:
            {
                std::lock_guard lock(inputMutex_);
                inputQueue_.emplace_back(type, std::move(buf));
            }
                buf.clear(); // moved from
                inputCv_.notify_one();
                break;
            case Msg::KeyframeReq:
                keyframeRequested_ = true;
                break;
            case Msg::Bye: // the user disconnected on the tablet: no reconnect is coming
                EndAs(SessionEnd::TabletLeft);
                stop_ = true;
                break;
            case Msg::FrameAck:
                OnFrameAck(buf, now);
                break;
            case Msg::Pong:
                OnPong(buf, now);
                break;
            case Msg::SecureText:
                OnSecureText(buf);
                break;
            default:
                break;
            }
        }
        LOGI("session: connection closed");
        EndAs(SessionEnd::Lost); // unless the tablet said goodbye, or the session was stopped
        stop_ = true;
        ackCv_.notify_all();
        inputCv_.notify_all();
    }

    void Session::OnFrameAck(const std::vector<uint8_t> &message, uint64_t now)
    {
        if (message.size() < 8)
            return;
        uint64_t pts = rd64(message.data());
        {
            std::lock_guard lock(inflightMutex_);
            while (!inflight_.empty() && inflight_.front().first < pts)
                inflight_.pop_front();
            if (!inflight_.empty() && inflight_.front().first == pts)
            {
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

    void Session::OnPong(const std::vector<uint8_t> &message, uint64_t now)
    {
        if (message.size() < 8)
            return;
        const uint64_t sentUs = rd64(message.data());
        if (sentUs == 0 || sentUs > now)
            return; // 0: the answer to a WaitingPing
        uint32_t rtt = uint32_t(std::min<uint64_t>(now - sentUs, UINT32_MAX));
        lastRttUs_ = rtt;
        rttSumUs_ += rtt;
        ++rttCount_;
    }

    // A password typed into a box on the tablet, to be typed into the locked sign-in screen (the workaround for when the tablet
    // cannot show that screen). Accepted only when lock-screen control is on AND the link carries no eavesdroppers: encrypted Wi-Fi
    // (TLS) or a USB cable. The injector types it only onto the sign-in desktop. The text is never logged, and the wire buffer and
    // the wide copy are the only copies: both are wiped as soon as it is handed off.
    void Session::OnSecureText(std::vector<uint8_t> &message)
    {
        if (lockControl_ && uacClick_ && !conn_.tls && !usb_)
            SendNotice("This PC will only accept the password over an encrypted connection. Connect over Wi-Fi with encryption on, or by USB.");
        else if (lockControl_ && uacClick_ && !message.empty())
        {
            const bool submit = (message[0] & SecureTextSubmit) != 0;
            const uint8_t *utf8 = message.data() + 1;
            size_t len = message.size() - 1;
            if (len > kMaxPasswordBytes)
            {
                len = kMaxPasswordBytes;
                // Don't cut in the middle of a UTF-8 sequence, or Widen would corrupt the last
                // character: back off while the first dropped byte is a continuation byte (10xxxxxx).
                while (len > 0 && (utf8[len] & 0xC0) == 0x80)
                    --len;
            }
            std::wstring text = Widen(std::string_view(reinterpret_cast<const char *>(utf8), len));
            uacClick_->TypeUnicode(text, submit);
            SecureZeroMemory(text.data(), text.size() * sizeof(wchar_t));
        }
        SecureZeroMemory(message.data(), message.size());
    }
    void Session::InputLoop()
    {
        MakeRealtimeThread(L"Games"); // input injection latency
        std::deque<std::pair<Msg, std::vector<uint8_t>>> batch;
        while (!stop_)
        {
            {
                std::unique_lock lock(inputMutex_);
                inputCv_.wait_for(lock, std::chrono::milliseconds(50), [&]
                                  { return stop_ || !inputQueue_.empty(); });
                batch.swap(inputQueue_);
            }
            for (auto &[type, buf] : batch)
            {
                switch (type)
                {
                case Msg::Touch:
                    input_->OnTouch(buf.data(), buf.size());
                    break;
                case Msg::Pen:
                    input_->OnPen(buf.data(), buf.size());
                    break;
                case Msg::Mouse:
                    input_->OnMouse(buf.data(), buf.size());
                    break;
                case Msg::MouseRel:
                    input_->OnMouseRel(buf.data(), buf.size());
                    break;
                case Msg::Key:
                    input_->OnKey(buf.data(), buf.size());
                    break;
                case Msg::Gamepad:
                    if (!gamepads_)
                        gamepads_ = std::make_unique<GamepadBridge>(
                            [this](uint8_t index, uint8_t largeMotor, uint8_t smallMotor)
                            {
                                const uint8_t rumble[3] = {index, largeMotor, smallMotor};
                                Send(Msg::Rumble, rumble, sizeof(rumble));
                            },
                            [this](const char *text)
                            { SendNotice(text); });
                    gamepads_->OnState(buf.data(), buf.size());
                    break;
                default:
                    break;
                }
            }
            batch.clear();
            input_->Tick();
        }
    }

} // namespace pd
