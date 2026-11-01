// PadDisplay by Nezucloud. Copyright (c) 2026 Nezucloud.
// SPDX-License-Identifier: MIT
#include "jitterbuffer.h"

#include <algorithm>

namespace pd
{
    namespace
    {

        constexpr uint32_t kInputRate = 48000; // what the tablet sends

        constexpr size_t kStartFill = 960;     // 20 ms: the cushion before playing starts
        constexpr size_t kMaxCushion = 3840;   // 80 ms
        constexpr size_t kCushionGrowth = 480; // 10 ms, after an underrun
        constexpr size_t kCushionShrink = 240; // 5 ms, slowly, once it is calm again
        constexpr uint64_t kCalmMs = 20000;    // no underrun for this long

        constexpr size_t kMaxFill = 5760; // 120 ms: above this the oldest audio is dropped, to cap the delay

        constexpr uint64_t kInterpolationSamples = 2;
        constexpr double kMaxSpeedUp = 0.02;        // inaudible for speech, the same correction the tablet's own audio playback uses
        constexpr double kFullSpeedUpExcess = 2400; // 50 ms above the target
        constexpr float kS16ToFloat = 1.0f / 32768.0f;

    } // namespace

    void JitterBuffer::Start(uint32_t outputRate, uint64_t nowMs)
    {
        step_ = double(kInputRate) / double(outputRate);
        target_ = kStartFill;
        lastCushionChangeMs_ = nowMs;
    }

    void JitterBuffer::Write(const int16_t *samples, size_t count)
    {
        std::lock_guard lock(mutex_);
        for (size_t i = 0; i < count; ++i)
            ring_[head_++ % kRing] = samples[i] * kS16ToFloat;
        const uint64_t fill = head_ - tail_;
        if (fill > kMaxFill)
        { // the audio side fell behind: drop the oldest, keep the delay bounded
            dropped_ += fill - kStartFill;
            tail_ = head_ - kStartFill;
        }
    }

    void JitterBuffer::Read(float *out, uint32_t frames, uint64_t nowMs)
    {
        std::lock_guard lock(mutex_);
        if (restart_.exchange(false))
            DropEverything();
        if (!playing_ && head_ - tail_ >= target_)
            playing_ = true;
        for (uint32_t i = 0; i < frames; ++i)
            out[i] = playing_ ? NextSample(nowMs) : 0.f;
        ShrinkCushionWhenCalm(nowMs);
    }

    void JitterBuffer::DropEverything()
    {
        tail_ = head_;
        playing_ = false;
        pos_ = 0;
    }

    float JitterBuffer::NextSample(uint64_t nowMs)
    {
        const uint64_t fill = head_ - tail_;
        const uint64_t skipped = uint64_t(pos_);
        if (fill < skipped + kInterpolationSamples)
        {
            OnUnderrun(nowMs);
            return 0.f;
        }
        const uint64_t first = tail_ + skipped;
        const float fraction = float(pos_ - double(skipped));
        const float sample = ring_[first % kRing] * (1 - fraction) + ring_[(first + 1) % kRing] * fraction;
        // Input and output run at the same rate, so a cushion left over from the start would stay for ever. Reading a little
        // faster while it is above the target works it off.
        const double excess = double(fill) - double(target_);
        const double speedUp = excess > 0 ? std::min(kMaxSpeedUp, excess / kFullSpeedUpExcess * kMaxSpeedUp) : 0.0;
        pos_ += step_ * (1.0 + speedUp);
        const uint64_t consumed = uint64_t(pos_);
        tail_ += consumed;
        pos_ -= double(consumed);
        return sample;
    }

    void JitterBuffer::OnUnderrun(uint64_t nowMs)
    {
        ++underruns_;
        playing_ = false;
        pos_ = 0;
        if (nowMs - lastCushionChangeMs_ < kCalmMs)
            target_ = std::min(target_ + kCushionGrowth, kMaxCushion);
        lastCushionChangeMs_ = nowMs;
    }

    void JitterBuffer::ShrinkCushionWhenCalm(uint64_t nowMs)
    {
        if (target_ <= kStartFill || nowMs - lastCushionChangeMs_ <= kCalmMs)
            return;
        target_ = std::max(kStartFill, target_ - kCushionShrink);
        lastCushionChangeMs_ = nowMs;
    }

} // namespace pd
