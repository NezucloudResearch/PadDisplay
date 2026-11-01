// PadDisplay by Nezucloud. Copyright (c) 2026 Nezucloud.
// SPDX-License-Identifier: MIT
// Between the network thread and an audio thread: the tablet's microphone, mono at 48 kHz as received, read out at the device's
// rate. The cushion grows after underruns, so a jittery Wi-Fi link ends up with a buffer that rides it out instead of
// crackling. Touches no device, so it can be tested alone.
#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

namespace pd
{

    class JitterBuffer
    {
    public:
        // Call before the first Read. nowMs, here and in Read: any millisecond clock, the same one each time.
        void Start(uint32_t outputRate, uint64_t nowMs);
        // Network thread. Waits at most for a Read that is running; a fixed ring, so no allocation per packet.
        void Write(const int16_t *samples, size_t count);
        // Audio thread. `out` gets `frames` mono samples at the output rate, silence while the cushion builds.
        void Read(float *out, uint32_t frames, uint64_t nowMs);
        // Any thread. The audio left is dropped and the next audio builds its cushion first, without that counting as an underrun.
        void Restart() { restart_ = true; }
        uint64_t Underruns() const { return underruns_; }
        uint64_t Dropped() const { return dropped_; }

    private:
        void DropEverything();
        float NextSample(uint64_t nowMs);
        void OnUnderrun(uint64_t nowMs);
        void ShrinkCushionWhenCalm(uint64_t nowMs);

        static constexpr size_t kRing = 24000; // 500 ms

        std::mutex mutex_; // held for one Write or one Read of a device buffer
        std::vector<float> ring_ = std::vector<float>(kRing);
        uint64_t head_ = 0, tail_ = 0; // total samples written / read; fill = head - tail

        // The audio thread's alone.
        double step_ = 1;   // input samples consumed per output sample
        double pos_ = 0;    // fractional read position (input samples), for the resampler
        size_t target_ = 0; // the cushion to build before playing
        bool playing_ = false;
        uint64_t lastCushionChangeMs_ = 0; // when an underrun happened or the cushion last shrank

        std::atomic<bool> restart_{false};
        std::atomic<uint64_t> underruns_{0}, dropped_{0};
    };

} // namespace pd
