// PadDisplay by Nezucloud. Copyright (c) 2026 Nezucloud.
// SPDX-License-Identifier: MIT
// Optional PC-audio-to-tablet stream. Runs on its own TCP connection (opened by the tablet with an
// AudioHello), so audio never waits behind video frames and costs nothing when not requested.
// Capture: WASAPI loopback of the default output device (taken before the PC's mute/volume), sent
// as 16-bit stereo PCM in ~10 ms packets. No codec: zero encode latency, ~1.5 Mbps.
#pragma once
#include "net/net.h"

#include <atomic>
#include <cstdint>
#include <vector>

namespace pd
{

    struct AudioLoopback;

    class AudioSession
    {
    public:
        AudioSession(Conn conn, bool mutePc);
        ~AudioSession();
        void Run(); // blocks until the tablet disconnects or Stop()
        void Stop();

    private:
        // Why a capture run ended. Only a lost device is worth another run.
        enum class StreamEnd
        {
            DeviceLost,
            TabletClosed,
            SendFailed,
            Stopped
        };

        StreamEnd Stream(); // one capture run on the current default device
        StreamEnd PumpUntilEnded(const AudioLoopback &loopback);
        bool SendFormat(uint32_t rate);
        bool SendOrDrop(const std::vector<uint8_t> &packet);
        bool PeerClosed();
        void WaitForStop(DWORD ms);
        static const char *Describe(StreamEnd end);

        Conn conn_;
        bool mutePc_;
        HANDLE stopEvent_; // wakes the waits between capture runs at once
        std::atomic<bool> stop_{false};
        uint64_t sentPackets_ = 0, droppedPackets_ = 0;
    };

} // namespace pd
