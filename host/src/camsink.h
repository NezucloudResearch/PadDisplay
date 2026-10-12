// PadDisplay by Nezucloud. Copyright (c) 2026 Nezucloud.
// SPDX-License-Identifier: MIT
// The tablet's camera as a PC webcam: receives the tablet's H.264 stream on its own connection, decodes it and hands the
// newest frame to PadDisplayCamera.dll through the shared mailbox (camshared.h). Only runs while a PC app has the
// webcam open: the tablet is told (CamState) to stop capturing the moment nobody is watching.
#pragma once
#include "net.h"
#include "settings.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>

namespace pd {

struct CamShared;

class CamSession {
public:
    using StatusFn = std::function<void(const std::wstring&)>;
    CamSession(Conn conn, const Settings& settings, uint16_t width, uint16_t height, uint8_t fps, StatusFn status);
    ~CamSession();
    void Run(); // blocks until the tablet disconnects or Stop()
    void Stop();

private:
    void SendNotice(const char* text);
    void CloseSocket();
    void SendState(bool active);
    void Watch(); // tells the tablet when a PC app opens or closes the webcam
    bool OpenMailbox();
    void Publish(const uint8_t* y, uint32_t strideY, const uint8_t* uv, uint32_t strideUv, uint32_t w, uint32_t h, int64_t ptsUs);

    Conn conn_;
    Settings settings_;
    uint16_t width_, height_;
    uint8_t fps_;
    StatusFn status_;
    std::atomic<bool> stop_{false};
    std::mutex sendMutex_;   // the receive thread asks for keyframes, the watcher thread sends the state
    std::mutex socketMutex_; // Stop() (another thread) and the end of Run() both touch the socket

    void* stopEvent_ = nullptr; // HANDLE: wakes the watcher at once when the session ends
    void* mapping_ = nullptr;   // HANDLE
    std::atomic<CamShared*> view_{nullptr};
    std::atomic<bool> active_{false}; // a PC app is reading the camera
    uint64_t framesIn_ = 0, framesOut_ = 0, decodeErrors_ = 0;
};

} // namespace pd
