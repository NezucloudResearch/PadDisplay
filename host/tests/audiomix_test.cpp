// PadDisplay by Nezucloud. Copyright (c) 2026 Nezucloud.
// SPDX-License-Identifier: MIT
#include "audio/audiomix.h"

#include <ks.h>
#include <ksmedia.h>
#include <cstdio>
#include <cstdlib>
#include <vector>

using pd::StereoMixer;

namespace
{

    int failures = 0;

    constexpr int16_t kFullScale = 32767;

    struct StereoSample
    {
        int16_t left, right;
    };

    WAVEFORMATEX PlainFormat(WORD tag, WORD channels, WORD bits)
    {
        WAVEFORMATEX format{};
        format.wFormatTag = tag;
        format.nChannels = channels;
        format.wBitsPerSample = bits;
        format.nSamplesPerSec = 48000;
        format.nBlockAlign = WORD(channels * bits / 8);
        return format;
    }

    WAVEFORMATEXTENSIBLE FloatFormatWithSpeakers(WORD channels, DWORD speakers)
    {
        WAVEFORMATEXTENSIBLE format{};
        format.Format = PlainFormat(WAVE_FORMAT_EXTENSIBLE, channels, 32);
        format.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
        format.dwChannelMask = speakers;
        format.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
        return format;
    }

    StereoSample MixOneFrame(const WAVEFORMATEX &format, const void *frame)
    {
        int16_t out[2];
        StereoMixer(format).ToStereoS16(static_cast<const BYTE *>(frame), 1, out);
        return {out[0], out[1]};
    }

    StereoSample MixFloats(const WAVEFORMATEXTENSIBLE &format, std::vector<float> frame) { return MixOneFrame(format.Format, frame.data()); }

    void ExpectNear(const char *test, int actual, int expected)
    {
        if (std::abs(actual - expected) <= 1)
            return;
        std::printf("FAILED %s: got %d, expected %d\n", test, actual, expected);
        ++failures;
    }

    void ExpectStereo(const char *test, StereoSample actual, int left, int right)
    {
        ExpectNear(test, actual.left, left);
        ExpectNear(test, actual.right, right);
    }

    int Scaled(float gain) { return int(gain * kFullScale); }

    void TestSixteenBitPcmStaysAsItIs()
    {
        const int16_t frame[2] = {16384, -16384};
        ExpectStereo("s16", MixOneFrame(PlainFormat(WAVE_FORMAT_PCM, 2, 16), frame), Scaled(0.5f), Scaled(-0.5f));
    }

    void TestTwentyFourBitPcmIsReadInThreeBytes()
    {
        const BYTE frame[6] = {0, 0, 0x40, 0, 0, 0xC0}; // +0.5, -0.5
        ExpectStereo("s24", MixOneFrame(PlainFormat(WAVE_FORMAT_PCM, 2, 24), frame), Scaled(0.5f), Scaled(-0.5f));
    }

    void TestThirtyTwoBitPcm()
    {
        const int32_t frame[2] = {0x40000000, -0x40000000};
        ExpectStereo("s32", MixOneFrame(PlainFormat(WAVE_FORMAT_PCM, 2, 32), frame), Scaled(0.5f), Scaled(-0.5f));
    }

    void TestEightBitPcmIsUnsigned()
    {
        const BYTE frame[2] = {192, 64}; // +0.5, -0.5 around the 128 midpoint
        ExpectStereo("u8", MixOneFrame(PlainFormat(WAVE_FORMAT_PCM, 2, 8), frame), Scaled(0.5f), Scaled(-0.5f));
    }

    void TestFloat32()
    {
        const float frame[2] = {0.5f, -0.5f};
        ExpectStereo("float32", MixOneFrame(PlainFormat(WAVE_FORMAT_IEEE_FLOAT, 2, 32), frame), Scaled(0.5f), Scaled(-0.5f));
    }

    void TestFloat64()
    {
        const double frame[2] = {0.5, -0.5};
        ExpectStereo("float64", MixOneFrame(PlainFormat(WAVE_FORMAT_IEEE_FLOAT, 2, 64), frame), Scaled(0.5f), Scaled(-0.5f));
    }

    void TestMonoGoesToBothSidesAtFullLevel()
    {
        const float frame[1] = {0.5f};
        ExpectStereo("mono", MixOneFrame(PlainFormat(WAVE_FORMAT_IEEE_FLOAT, 1, 32), frame), Scaled(0.5f), Scaled(0.5f));
    }

    void TestLoudSamplesAreClippedNotWrapped()
    {
        const float frame[2] = {3.f, -3.f};
        ExpectStereo("clip", MixOneFrame(PlainFormat(WAVE_FORMAT_IEEE_FLOAT, 2, 32), frame), kFullScale, -kFullScale);
    }

    void TestFivePointOneCentreReachesBothSides()
    {
        const auto format = FloatFormatWithSpeakers(6, KSAUDIO_SPEAKER_5POINT1);
        ExpectStereo("5.1 centre", MixFloats(format, {0, 0, 1, 0, 0, 0}), Scaled(0.707f), Scaled(0.707f));
    }

    void TestFivePointOneSubwooferIsDropped()
    {
        const auto format = FloatFormatWithSpeakers(6, KSAUDIO_SPEAKER_5POINT1);
        ExpectStereo("5.1 lfe", MixFloats(format, {0, 0, 0, 1, 0, 0}), 0, 0);
    }

    void TestFivePointOneBackChannelsStayOnTheirSide()
    {
        const auto format = FloatFormatWithSpeakers(6, KSAUDIO_SPEAKER_5POINT1);
        ExpectStereo("5.1 back left", MixFloats(format, {0, 0, 0, 0, 1, 0}), Scaled(0.5f), 0);
        ExpectStereo("5.1 back right", MixFloats(format, {0, 0, 0, 0, 0, 1}), 0, Scaled(0.5f));
    }

    void TestSevenPointOneSideChannelsStayOnTheirSide()
    {
        const auto format = FloatFormatWithSpeakers(8, KSAUDIO_SPEAKER_7POINT1_SURROUND);
        ExpectStereo("7.1 side left", MixFloats(format, {0, 0, 0, 0, 0, 0, 1, 0}), Scaled(0.5f), 0);
        ExpectStereo("7.1 side right", MixFloats(format, {0, 0, 0, 0, 0, 0, 0, 1}), 0, Scaled(0.5f));
    }

    void TestQuadBackChannelsAreNotTakenForCentreAndSubwoofer()
    {
        const auto format = FloatFormatWithSpeakers(4, KSAUDIO_SPEAKER_QUAD);
        ExpectStereo("quad back left", MixFloats(format, {0, 0, 1, 0}), Scaled(0.5f), 0);
        ExpectStereo("quad back right", MixFloats(format, {0, 0, 0, 1}), 0, Scaled(0.5f));
    }

    void TestThirdChannelOfTwoPointOneIsTheSubwoofer()
    {
        const auto format = FloatFormatWithSpeakers(3, KSAUDIO_SPEAKER_STEREO | SPEAKER_LOW_FREQUENCY);
        ExpectStereo("2.1 lfe", MixFloats(format, {0, 0, 1}), 0, 0);
    }

    void TestWithoutSpeakerNamesTheUsualOrderApplies()
    {
        const auto format = FloatFormatWithSpeakers(6, 0);
        ExpectStereo("no mask centre", MixFloats(format, {0, 0, 1, 0, 0, 0}), Scaled(0.707f), Scaled(0.707f));
        ExpectStereo("no mask lfe", MixFloats(format, {0, 0, 0, 1, 0, 0}), 0, 0);
    }

    void TestSpeakerAllMaskLeavesNoChannelSilent()
    {
        const auto format = FloatFormatWithSpeakers(2, SPEAKER_ALL);
        ExpectStereo("SPEAKER_ALL", MixFloats(format, {0.5f, -0.5f}), Scaled(0.5f), Scaled(-0.5f));
    }

    void TestChannelsBeyondTheNamedSpeakersAlternate()
    {
        const auto format = FloatFormatWithSpeakers(4, KSAUDIO_SPEAKER_STEREO);
        ExpectStereo("unnamed left", MixFloats(format, {0, 0, 1, 0}), Scaled(0.5f), 0);
        ExpectStereo("unnamed right", MixFloats(format, {0, 0, 0, 1}), 0, Scaled(0.5f));
    }

    void TestEveryFrameOfABufferIsConverted()
    {
        const float frames[4] = {0.25f, -0.25f, 0.5f, -0.5f};
        int16_t out[4];
        StereoMixer(PlainFormat(WAVE_FORMAT_IEEE_FLOAT, 2, 32)).ToStereoS16(reinterpret_cast<const BYTE *>(frames), 2, out);
        ExpectStereo("second frame", {out[2], out[3]}, Scaled(0.5f), Scaled(-0.5f));
    }

} // namespace

int main()
{
    TestSixteenBitPcmStaysAsItIs();
    TestTwentyFourBitPcmIsReadInThreeBytes();
    TestThirtyTwoBitPcm();
    TestEightBitPcmIsUnsigned();
    TestFloat32();
    TestFloat64();
    TestMonoGoesToBothSidesAtFullLevel();
    TestLoudSamplesAreClippedNotWrapped();
    TestFivePointOneCentreReachesBothSides();
    TestFivePointOneSubwooferIsDropped();
    TestFivePointOneBackChannelsStayOnTheirSide();
    TestSevenPointOneSideChannelsStayOnTheirSide();
    TestQuadBackChannelsAreNotTakenForCentreAndSubwoofer();
    TestThirdChannelOfTwoPointOneIsTheSubwoofer();
    TestWithoutSpeakerNamesTheUsualOrderApplies();
    TestSpeakerAllMaskLeavesNoChannelSilent();
    TestChannelsBeyondTheNamedSpeakersAlternate();
    TestEveryFrameOfABufferIsConverted();
    if (failures == 0)
        std::puts("audiomix: all tests passed");
    return failures == 0 ? 0 : 1;
}
