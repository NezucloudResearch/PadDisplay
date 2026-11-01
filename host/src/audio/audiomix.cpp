// PadDisplay by Nezucloud. Copyright (c) 2026 Nezucloud.
// SPDX-License-Identifier: MIT
#include "audiomix.h"

#include <ks.h>
#include <ksmedia.h>

namespace pd
{
    namespace
    {

        constexpr float kCentreGain = 0.707f; // -3 dB: it reaches both sides
        constexpr float kSurroundGain = 0.5f;

        constexpr DWORD kCentres = SPEAKER_FRONT_CENTER | SPEAKER_BACK_CENTER | SPEAKER_TOP_CENTER | SPEAKER_TOP_FRONT_CENTER | SPEAKER_TOP_BACK_CENTER;
        constexpr DWORD kLeftSurrounds = SPEAKER_BACK_LEFT | SPEAKER_SIDE_LEFT | SPEAKER_FRONT_LEFT_OF_CENTER | SPEAKER_TOP_FRONT_LEFT | SPEAKER_TOP_BACK_LEFT;
        constexpr DWORD kRightSurrounds = SPEAKER_BACK_RIGHT | SPEAKER_SIDE_RIGHT | SPEAKER_FRONT_RIGHT_OF_CENTER | SPEAKER_TOP_FRONT_RIGHT | SPEAKER_TOP_BACK_RIGHT;
        constexpr DWORD kNamedSpeakers = (SPEAKER_TOP_BACK_RIGHT << 1) - 1; // the higher bits are reserved, or SPEAKER_ALL
        constexpr DWORD kUsualFrontSpeakers = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT | SPEAKER_FRONT_CENTER | SPEAKER_LOW_FREQUENCY;

        // The channels of a frame are in the order of their speaker bits, lowest first; a format may name fewer speakers than it has channels.
        DWORD NamedSpeakers(const WAVEFORMATEX &format)
        {
            if (format.wFormatTag != WAVE_FORMAT_EXTENSIBLE)
                return kUsualFrontSpeakers;
            const DWORD named = reinterpret_cast<const WAVEFORMATEXTENSIBLE &>(format).dwChannelMask & kNamedSpeakers;
            return named ? named : kUsualFrontSpeakers;
        }

    } // namespace

    bool IsFloatFormat(const WAVEFORMATEX &format)
    {
        if (format.wFormatTag == WAVE_FORMAT_EXTENSIBLE)
            return reinterpret_cast<const WAVEFORMATEXTENSIBLE &>(format).SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
        return format.wFormatTag == WAVE_FORMAT_IEEE_FLOAT;
    }

    StereoMixer::StereoMixer(const WAVEFORMATEX &format)
        : reader_(ReaderFor(format)), frameBytes_(format.nBlockAlign), gains_(GainsFor(format)) {}

    void StereoMixer::ToStereoS16(const BYTE *frames, uint32_t frameCount, int16_t *out) const
    {
        for (uint32_t i = 0; i < frameCount; ++i)
        {
            const BYTE *sample = frames + size_t(i) * frameBytes_;
            float left = 0, right = 0;
            for (const ChannelGain &gain : gains_)
            {
                const float value = reader_.Read(sample);
                left += gain.left * value;
                right += gain.right * value;
                sample += reader_.bytes;
            }
            out[2 * i] = ToS16(left);
            out[2 * i + 1] = ToS16(right);
        }
    }

    StereoMixer::SampleReader StereoMixer::ReaderFor(const WAVEFORMATEX &format)
    {
        SampleReader reader;
        reader.bytes = format.wBitsPerSample / 8; // the container size: 24-bit audio in 32-bit containers is read as 32-bit
        reader.isFloat = IsFloatFormat(format);
        return reader;
    }

    float StereoMixer::SampleReader::Read(const BYTE *p) const
    {
        if (isFloat)
            return bytes == 8 ? float(*reinterpret_cast<const double *>(p)) : *reinterpret_cast<const float *>(p);
        switch (bytes)
        {
        case 1:
            return (int(p[0]) - 128) / 128.f; // 8-bit PCM is the one unsigned format
        case 2:
            return *reinterpret_cast<const int16_t *>(p) / 32768.f;
        case 3:
            return float(int32_t(uint32_t(p[0]) << 8 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 24)) / 2147483648.f;
        default:
            return float(*reinterpret_cast<const int32_t *>(p)) / 2147483648.f;
        }
    }

    std::vector<StereoMixer::ChannelGain> StereoMixer::GainsFor(const WAVEFORMATEX &format)
    {
        if (format.nChannels == 1)
            return {{1, 1}};
        std::vector<ChannelGain> gains;
        DWORD speakers = NamedSpeakers(format);
        for (int channel = 0; channel < format.nChannels; ++channel)
        {
            const DWORD speaker = speakers & (~speakers + 1); // the lowest bit still set
            speakers ^= speaker;
            const bool odd = channel % 2 != 0;
            if (speaker)
                gains.push_back(GainFor(speaker));
            else
                gains.push_back({odd ? 0 : kSurroundGain, odd ? kSurroundGain : 0}); // a channel without a speaker: alternately left and right
        }
        return gains;
    }

    StereoMixer::ChannelGain StereoMixer::GainFor(DWORD speaker)
    {
        if (speaker == SPEAKER_FRONT_LEFT)
            return {1, 0};
        if (speaker == SPEAKER_FRONT_RIGHT)
            return {0, 1};
        if (speaker & kCentres)
            return {kCentreGain, kCentreGain};
        if (speaker & kLeftSurrounds)
            return {kSurroundGain, 0};
        if (speaker & kRightSurrounds)
            return {0, kSurroundGain};
        return {0, 0}; // the subwoofer channel is left out
    }

} // namespace pd
