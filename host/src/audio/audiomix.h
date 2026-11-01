// PadDisplay by Nezucloud. Copyright (c) 2026 Nezucloud.
// SPDX-License-Identifier: MIT
// Turns audio in the device's mix format into the 16-bit stereo the tablet plays. Touches no device, so it can be tested alone.
#pragma once
#include <windows.h>
#include <mmreg.h>
#include <algorithm>
#include <cstdint>
#include <vector>

namespace pd
{

    bool IsFloatFormat(const WAVEFORMATEX &format);

    inline int16_t ToS16(float v) { return int16_t(std::clamp(v, -1.f, 1.f) * 32767.f); }

    class StereoMixer
    {
    public:
        explicit StereoMixer(const WAVEFORMATEX &format);
        // `out` has room for 2 * frameCount samples. Runs on the audio thread: no allocation.
        void ToStereoS16(const BYTE *frames, uint32_t frameCount, int16_t *out) const;

    private:
        struct SampleReader
        {
            bool isFloat = false;
            int bytes = 4;
            float Read(const BYTE *p) const;
        };
        struct ChannelGain
        {
            float left, right;
        };

        static SampleReader ReaderFor(const WAVEFORMATEX &format);
        static ChannelGain GainFor(DWORD speaker);
        static std::vector<ChannelGain> GainsFor(const WAVEFORMATEX &format);

        SampleReader reader_;
        int frameBytes_;
        std::vector<ChannelGain> gains_; // one per channel of the device format, computed once for the stream
    };

} // namespace pd
