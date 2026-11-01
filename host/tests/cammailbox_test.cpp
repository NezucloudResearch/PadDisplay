// PadDisplay by Nezucloud. Copyright (c) 2026 Nezucloud.
// SPDX-License-Identifier: MIT
#include "camera/camshared.h"
#include "camera/cammailbox.h"

#include <cstdio>
#include <vector>

using namespace pd;

namespace
{

    int failures = 0;

    constexpr uint32_t kWidth = 4, kHeight = 4;
    constexpr uint32_t kPaddedStride = 8;
    constexpr uint8_t kPadding = 0xEE;

    void Expect(const char *test, bool passed)
    {
        if (passed)
            return;
        std::printf("FAILED %s\n", test);
        ++failures;
    }

    // The mailbox's memory, as the Frame Server's mapping provides it: zeroed and big enough for any frame.
    class Slot
    {
    public:
        Slot() : memory_(kCamMapBytes / sizeof(uint64_t) + 1) {}
        CamShared &Get() { return *reinterpret_cast<CamShared *>(memory_.data()); }
        uint8_t *Pixels() { return CamPixels(&Get()); }

    private:
        std::vector<uint64_t> memory_;
    };

    // A frame whose rows are padded the way the decoder pads them, with every pixel numbered so a misplaced one shows.
    struct PaddedFrame
    {
        std::vector<uint8_t> y = std::vector<uint8_t>(size_t(kPaddedStride) * kHeight, kPadding);
        std::vector<uint8_t> uv = std::vector<uint8_t>(size_t(kPaddedStride) * kHeight / 2, kPadding);

        PaddedFrame()
        {
            for (uint32_t row = 0; row < kHeight; ++row)
                for (uint32_t x = 0; x < kWidth; ++x)
                    y[size_t(row) * kPaddedStride + x] = uint8_t(row * kWidth + x);
            for (uint32_t row = 0; row < kHeight / 2; ++row)
                for (uint32_t x = 0; x < kWidth; ++x)
                    uv[size_t(row) * kPaddedStride + x] = uint8_t(100 + row * kWidth + x);
        }

        Nv12Frame View() const { return {y.data(), uv.data(), kPaddedStride, kWidth, kHeight, 1234}; }
    };

    void WriteFramePacksTheLumaRows()
    {
        Slot slot;
        const PaddedFrame frame;
        WriteFrame(slot.Get(), frame.View());
        for (uint32_t i = 0; i < kWidth * kHeight; ++i)
            Expect("luma is packed without its padding", slot.Pixels()[i] == i);
    }

    void WriteFramePutsChromaRightAfterTheLuma()
    {
        Slot slot;
        const PaddedFrame frame;
        WriteFrame(slot.Get(), frame.View());
        const uint8_t *uv = slot.Pixels() + kWidth * kHeight;
        for (uint32_t i = 0; i < kWidth * kHeight / 2; ++i)
            Expect("chroma follows the luma without padding", uv[i] == 100 + i);
    }

    void WriteFrameDescribesTheFrame()
    {
        Slot slot;
        const PaddedFrame frame;
        WriteFrame(slot.Get(), frame.View());
        Expect("the size is recorded", slot.Get().width == int32_t(kWidth) && slot.Get().height == int32_t(kHeight));
        Expect("the timestamp is recorded", slot.Get().ptsUs == 1234);
        Expect("the write time is recorded", slot.Get().writeTick != 0);
    }

    void WriteFrameLeavesTheSequenceEvenAndAdvanced()
    {
        Slot slot;
        const PaddedFrame frame;
        WriteFrame(slot.Get(), frame.View());
        const int64_t first = slot.Get().seq;
        WriteFrame(slot.Get(), frame.View());
        Expect("the sequence is even after a write", first % 2 == 0);
        Expect("each write advances the sequence", slot.Get().seq == first + 2);
    }

    void WriteFrameRecoversAnOddSequenceFromADeadHost()
    {
        Slot slot;
        const PaddedFrame frame;
        slot.Get().seq = 1001;
        WriteFrame(slot.Get(), frame.View());
        Expect("an inherited odd sequence ends up even", slot.Get().seq == 1002);
    }

    void WriteFrameTakesTheLargestFrame()
    {
        Slot slot;
        const std::vector<uint8_t> y(size_t(kCamMaxWidth) * kCamMaxHeight, 1), uv(size_t(kCamMaxWidth) * kCamMaxHeight / 2, 2);
        const Nv12Frame frame{y.data(), uv.data(), kCamMaxWidth, kCamMaxWidth, kCamMaxHeight, 0};
        Expect("1920x1080 fits", WriteFrame(slot.Get(), frame));
        Expect("the last chroma byte is inside the slot", slot.Pixels()[kCamCapacity - 1] == 2);
    }

    void WriteFrameRefusesWhatDoesNotFit()
    {
        Slot slot;
        const std::vector<uint8_t> y(size_t(kCamMaxWidth + 2) * (kCamMaxHeight + 2), 1), uv(y.size() / 2, 2);
        const uint32_t pitch = kCamMaxWidth + 2;
        Expect("too wide", !WriteFrame(slot.Get(), {y.data(), uv.data(), pitch, kCamMaxWidth + 2, kCamMaxHeight, 0}));
        Expect("too high", !WriteFrame(slot.Get(), {y.data(), uv.data(), pitch, kCamMaxWidth, kCamMaxHeight + 2, 0}));
        Expect("a refused frame leaves the sequence alone", slot.Get().seq == 0);
    }

    void ReaderIsRecentAtTheStaleBoundary()
    {
        constexpr uint64_t now = 100000;
        const int64_t askedAt = int64_t(now - kCamReaderStaleMs);
        Expect("one millisecond short of stale is recent", ReaderIsRecent(askedAt + 1, now));
        Expect("exactly stale is gone", !ReaderIsRecent(askedAt, now));
    }

    void ReaderIsRecentWhenItsTickIsAheadOfOurs()
    {
        constexpr uint64_t now = 100000;
        Expect("a tick stamped just after our clock read is recent", ReaderIsRecent(int64_t(now) + 16, now));
    }

    void ReaderIsNotRecentWhenItNeverAsked()
    {
        Expect("no reader has written the tick yet", !ReaderIsRecent(0, 100000));
    }

} // namespace

int main()
{
    WriteFramePacksTheLumaRows();
    WriteFramePutsChromaRightAfterTheLuma();
    WriteFrameDescribesTheFrame();
    WriteFrameLeavesTheSequenceEvenAndAdvanced();
    WriteFrameRecoversAnOddSequenceFromADeadHost();
    WriteFrameTakesTheLargestFrame();
    WriteFrameRefusesWhatDoesNotFit();
    ReaderIsRecentAtTheStaleBoundary();
    ReaderIsRecentWhenItsTickIsAheadOfOurs();
    ReaderIsNotRecentWhenItNeverAsked();
    if (failures == 0)
        std::puts("cammailbox: all tests passed");
    return failures == 0 ? 0 : 1;
}
