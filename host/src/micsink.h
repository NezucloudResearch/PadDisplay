// PadDisplay by Nezucloud. Copyright (c) 2026 Nezucloud.
// SPDX-License-Identifier: MIT
// The tablet's microphone as a PC microphone. The audio goes to one of two virtual microphones, chosen by micbackend.h:
// PadDisplay's own driver (the acxmic repository, at host\third_party\padmic), which shows up in Windows as a recording
// device, or VB-CABLE (cablesink.h) where that driver cannot load. Apps on the PC pick the microphone it provides.
#pragma once
#include "micuse.h"
#include "net.h"
#include "settings.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>

namespace pd {

class MicSession {
public:
    using StatusFn = std::function<void(const std::wstring&)>;
    MicSession(Conn conn, const Settings& settings, StatusFn status);
    ~MicSession();
    void Run(); // blocks until the tablet disconnects or Stop()
    void Stop();

private:
    void SendNotice(const char* text);
    // Tells the tablet whether an app on the PC records from the virtual microphone: its own microphone only runs while one does.
    void SendState(bool recording);
    // Starts telling the tablet, from now until `watcher` is stopped. A device that cannot be watched (empty ID, or Windows
    // refuses) counts as always in use: the microphone then works as it did before the tablet could be told.
    void WatchRecording(RecordingWatcher& watcher, const std::wstring& deviceId, const std::function<void(bool)>& onChange);
    void CloseSocket();
    // Receives MicData until the tablet goes or Stop(); every packet's audio (48 kHz mono s16le, with its byte count) goes to
    // `sink`, which returns false when the output is gone. true = the output failed.
    bool Pump(const std::function<bool(const uint8_t*, size_t)>& sink);
    void RunDriver(const std::function<void(const char*)>& refuse);
    void RunCable(const std::function<void(const char*)>& refuse);
    // Gives the driver the audio. false = the driver is gone (removed or disabled while we were running).
    bool WriteToDriver(void* device, const uint8_t* pcm, size_t bytes);

    Conn conn_;
    Settings settings_;
    StatusFn status_;
    std::atomic<bool> stop_{false};
    std::mutex socketMutex_; // Stop() (another thread) and the end of Run() both touch the socket
    std::mutex sendMutex_;   // the watcher's thread sends MicState, the session's own thread sends Notice
    uint64_t packets_ = 0, lateWrites_ = 0;
};

} // namespace pd
