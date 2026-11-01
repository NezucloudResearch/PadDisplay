// PadDisplay by Nezucloud. Copyright (c) 2026 Nezucloud.
// SPDX-License-Identifier: MIT
// The tablet's camera as a PC webcam: receives the tablet's H.264 stream on its own connection, decodes it and hands the
// newest frame to PadDisplayCamera.dll through the shared mailbox (camshared.h). The tablet only captures while a PC app
// has the webcam open: it is told (CamState) to stop the moment nobody is watching.
#pragma once
#include "cammailbox.h"
#include "net/net.h"
#include "settings.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace pd
{

    class CamDecoder;

    class CamSession
    {
    public:
        using StatusFn = std::function<void(const std::wstring &)>;
        CamSession(Conn conn, const Settings &settings, uint16_t width, uint16_t height, uint8_t fps, StatusFn status);
        ~CamSession();
        void Run(); // blocks until the tablet disconnects or Stop()
        void Stop();

    private:
        const char *WhyRefused() const; // what to tell the tablet; null = nothing stands in the way
        void DecodeUntilEnded();
        void ReceiveStream(CamDecoder &decoder);
        void OnConfig(CamDecoder &decoder, const std::vector<uint8_t> &config);
        void OnFrame(CamDecoder &decoder, const std::vector<uint8_t> &message);
        void RecoverFromDecodeError(CamDecoder &decoder);
        void Watch(); // tells the tablet when a PC app opens or closes the webcam
        void SendNotice(const char *text);
        void SendState(bool active);
        void CloseSocket();

        Conn conn_;
        Settings settings_;
        uint16_t width_, height_;
        uint8_t fps_;
        StatusFn status_;
        std::atomic<bool> stop_{false};
        std::mutex sendMutex_;   // the receive thread asks for keyframes, the watcher thread sends the state
        std::mutex socketMutex_; // Stop() (another thread) and the end of Run() both touch the socket

        HANDLE stopEvent_ = nullptr; // wakes the watcher at once when the session ends
        CamMailbox mailbox_;
        std::atomic<bool> active_{false}; // a PC app is reading the camera
        bool needKey_ = true;             // the decoder has nothing to decode against; the receive thread's own
        uint64_t framesIn_ = 0, framesOut_ = 0, decodeErrors_ = 0;
    };

} // namespace pd
