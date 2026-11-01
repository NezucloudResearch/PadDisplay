// PadDisplay by Nezucloud. Copyright (c) 2026 Nezucloud.
// SPDX-License-Identifier: MIT
#include "audio/jitterbuffer.h"

#include <cmath>
#include <cstdio>
#include <vector>

using pd::JitterBuffer;

namespace
{

    int failures = 0;

    constexpr uint32_t kSameRate = 48000;
    constexpr uint32_t kStartCushion = 960;
    constexpr uint64_t kCalmMs = 20000;
    constexpr float kOneStep = 1.0f / 32768.0f;

    void Expect(const char *test, bool passed)
    {
        if (passed)
            return;
        std::printf("FAILED %s\n", test);
        ++failures;
    }

    // Writes `count` samples whose values count up from `first`, so a read shows exactly which sample it returned.
    void WriteRamp(JitterBuffer &buffer, size_t count, int first = 1)
    {
        std::vector<int16_t> samples(count);
        for (size_t i = 0; i < count; ++i)
            samples[i] = int16_t((first + int(i)) % 30000);
        buffer.Write(samples.data(), count);
    }

    std::vector<float> ReadSamples(JitterBuffer &buffer, uint32_t frames, uint64_t nowMs = 0)
    {
        std::vector<float> out(frames, -1.f);
        buffer.Read(out.data(), frames, nowMs);
        return out;
    }

    bool AllSilent(const std::vector<float> &samples)
    {
        for (float sample : samples)
            if (sample != 0.f)
                return false;
        return true;
    }

    constexpr size_t kLeftAfterUnderrun = 1; // interpolating needs two samples, so the last one stays behind

    // Reads past the end of the buffer at `nowMs`, which counts one underrun.
    void ForceUnderrun(JitterBuffer &buffer, uint64_t nowMs)
    {
        ReadSamples(buffer, 8000, nowMs);
    }

    void TestStaysSilentUntilTheCushionIsThere()
    {
        JitterBuffer buffer;
        buffer.Start(kSameRate, 0);
        WriteRamp(buffer, kStartCushion - 1);
        Expect("silent below the cushion", AllSilent(ReadSamples(buffer, 100)));
    }

    void TestPlaysSamplesInOrderAtTheSameRate()
    {
        JitterBuffer buffer;
        buffer.Start(kSameRate, 0);
        WriteRamp(buffer, kStartCushion);
        const std::vector<float> out = ReadSamples(buffer, 100);
        Expect("first sample", std::fabs(out[0] - kOneStep) < kOneStep / 100);
        Expect("hundredth sample", std::fabs(out[99] - 100 * kOneStep) < kOneStep / 100);
    }

    void TestResamplesFromTheInputRate()
    {
        JitterBuffer buffer;
        buffer.Start(96000, 0);
        WriteRamp(buffer, kStartCushion);
        const std::vector<float> out = ReadSamples(buffer, 3);
        Expect("half way between two samples", std::fabs(out[1] - 1.5f * kOneStep) < kOneStep / 100);
    }

    void TestWorksOffACushionAboveTheTarget()
    {
        JitterBuffer buffer;
        buffer.Start(kSameRate, 0);
        WriteRamp(buffer, kStartCushion + 2400);
        const std::vector<float> out = ReadSamples(buffer, 1000);
        Expect("reads faster than it plays", out[999] > 1010 * kOneStep);
    }

    void TestRunningDryCountsAnUnderrunAndGoesSilent()
    {
        JitterBuffer buffer;
        buffer.Start(kSameRate, 0);
        WriteRamp(buffer, kStartCushion);
        ForceUnderrun(buffer, 0);
        Expect("one underrun", buffer.Underruns() == 1);
        Expect("silent after running dry", AllSilent(ReadSamples(buffer, 100)));
    }

    void TestAnUnderrunSoonAfterAnotherGrowsTheCushion()
    {
        JitterBuffer buffer;
        buffer.Start(kSameRate, 0);
        WriteRamp(buffer, kStartCushion);
        ForceUnderrun(buffer, 1);
        WriteRamp(buffer, kStartCushion + 479 - kLeftAfterUnderrun);
        Expect("needs 10 ms more", AllSilent(ReadSamples(buffer, 10, 2)));
        WriteRamp(buffer, 1);
        Expect("plays with the bigger cushion", !AllSilent(ReadSamples(buffer, 10, 3)));
    }

    void TestTheCushionNeverGrowsPastItsLimit()
    {
        JitterBuffer buffer;
        buffer.Start(kSameRate, 0);
        uint64_t nowMs = 0;
        for (int underrun = 0; underrun < 8; ++underrun)
        { // each round fills what even the largest cushion needs
            WriteRamp(buffer, 3840);
            ForceUnderrun(buffer, ++nowMs);
        }
        WriteRamp(buffer, 3840);
        Expect("80 ms is enough", !AllSilent(ReadSamples(buffer, 10, nowMs)));
    }

    void TestTheCushionShrinksOnceItIsCalm()
    {
        JitterBuffer buffer;
        buffer.Start(kSameRate, 0);
        WriteRamp(buffer, kStartCushion);
        ForceUnderrun(buffer, 1); // cushion 20 + 10 ms
        ReadSamples(buffer, 1, 1 + kCalmMs + 1);
        WriteRamp(buffer, kStartCushion + 239 - kLeftAfterUnderrun);
        Expect("still 25 ms", AllSilent(ReadSamples(buffer, 10, 1 + kCalmMs + 2)));
        WriteRamp(buffer, 1);
        Expect("shrunk by 5 ms", !AllSilent(ReadSamples(buffer, 10, 1 + kCalmMs + 3)));
    }

    void TestAnOldUnderrunDoesNotGrowTheCushion()
    {
        JitterBuffer buffer;
        buffer.Start(kSameRate, 0);
        WriteRamp(buffer, kStartCushion);
        ForceUnderrun(buffer, kCalmMs);
        WriteRamp(buffer, kStartCushion);
        Expect("cushion unchanged", !AllSilent(ReadSamples(buffer, 10, kCalmMs + 1)));
    }

    void TestTooMuchAudioDropsTheOldestDownToTheCushion()
    {
        JitterBuffer buffer;
        buffer.Start(kSameRate, 0);
        WriteRamp(buffer, 5761);
        Expect("dropped all but the cushion", buffer.Dropped() == 5761 - kStartCushion);
        const std::vector<float> out = ReadSamples(buffer, 1);
        Expect("keeps the newest", std::fabs(out[0] - (5761 - kStartCushion + 1) * kOneStep) < kOneStep / 10);
    }

    void TestRestartDropsTheAudioWithoutAnUnderrun()
    {
        JitterBuffer buffer;
        buffer.Start(kSameRate, 0);
        WriteRamp(buffer, kStartCushion);
        buffer.Restart();
        Expect("silent after a restart", AllSilent(ReadSamples(buffer, 100)));
        Expect("no underrun", buffer.Underruns() == 0);
    }

    void TestTheRingWrapsAround()
    {
        JitterBuffer buffer;
        buffer.Start(kSameRate, 0);
        WriteRamp(buffer, kStartCushion);
        int nextToWrite = kStartCushion + 1;
        int samplesRead = 0;
        float last = 0;
        for (int packet = 0; packet < 1234; ++packet)
        { // 2.5 times through the ring; refilled to the target, so no speed-up
            last = ReadSamples(buffer, 100).back();
            samplesRead += 100;
            WriteRamp(buffer, 100, nextToWrite);
            nextToWrite += 100;
        }
        Expect("still in order after wrapping", std::fabs(last - (samplesRead % 30000) * kOneStep) < kOneStep / 100);
    }

} // namespace

int main()
{
    TestStaysSilentUntilTheCushionIsThere();
    TestPlaysSamplesInOrderAtTheSameRate();
    TestResamplesFromTheInputRate();
    TestWorksOffACushionAboveTheTarget();
    TestRunningDryCountsAnUnderrunAndGoesSilent();
    TestAnUnderrunSoonAfterAnotherGrowsTheCushion();
    TestTheCushionNeverGrowsPastItsLimit();
    TestTheCushionShrinksOnceItIsCalm();
    TestAnOldUnderrunDoesNotGrowTheCushion();
    TestTooMuchAudioDropsTheOldestDownToTheCushion();
    TestRestartDropsTheAudioWithoutAnUnderrun();
    TestTheRingWrapsAround();
    if (failures == 0)
        std::puts("jitterbuffer: all tests passed");
    return failures == 0 ? 0 : 1;
}
