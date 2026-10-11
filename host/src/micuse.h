// PadDisplay by Nezucloud. Copyright (c) 2026 Nezucloud.
// SPDX-License-Identifier: MIT
// Is an app on the PC recording from the virtual microphone right now? The tablet's own microphone only runs while one is
// (like its camera), so Android does not show the microphone in use for the whole session.
#pragma once
#include <functional>
#include <string>
#include <thread>

namespace pd {

// Watches one recording device. Windows keeps an audio session per app that opened the device and reports when a session
// starts or stops its stream, so this waits on those notifications: no polling, and no wake-up while nothing changes.
class RecordingWatcher {
public:
    using Changed = std::function<void(bool recording)>;

    RecordingWatcher();
    ~RecordingWatcher();
    // deviceId: an endpoint ID (see RecordingDeviceId in cablesink.h). `changed` is called on the watcher's own thread: once
    // with the state at the start, then on every change. A start is passed on at once; a stop only after the device was
    // idle for 2 s, because apps close and reopen a microphone while they look at the devices.
    // false = the device's sessions cannot be watched; the caller then has to treat the microphone as always in use.
    bool Start(const std::wstring& deviceId, Changed changed);
    void Stop();

private:
    void Run(std::wstring deviceId);

    Changed changed_;
    std::thread thread_;
    void* stop_ = nullptr;    // event handles
    void* started_ = nullptr; // signalled once Run knows whether it can watch
    bool watching_ = false;   // written by Run before it signals started_
};

} // namespace pd
