// PadDisplay by Nezucloud. Copyright (c) 2026 Nezucloud.
// SPDX-License-Identifier: MIT
// Optional PC-audio-to-tablet stream. Runs on its own TCP connection (opened by the tablet with an
// AudioHello), so audio never waits behind video frames and costs nothing when not requested.
// Capture: WASAPI loopback of the default output device (taken before the PC's mute/volume), sent
// as 16-bit stereo PCM in ~5 ms packets. No codec: zero encode latency, ~1.5 Mbps.
#pragma once
#include <winsock2.h>
#include <atomic>
#include <cstdint>

namespace pd {

class AudioSession {
public:
    AudioSession(SOCKET sock, bool mutePc);
    ~AudioSession();
    void Run(); // blocks until the tablet disconnects or Stop()
    void Stop();

private:
    bool Stream(); // one capture run on the current default device; false = connection over
    bool PeerClosed();

    SOCKET sock_;
    bool mutePc_;
    std::atomic<bool> stop_{false};
    uint64_t sentPackets_ = 0, droppedPackets_ = 0;
};

} // namespace pd
