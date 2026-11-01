// PadDisplay by Nezucloud. Copyright (c) 2026 Nezucloud.
// SPDX-License-Identifier: MIT
#include "frame_reader.h"

#include <cstdio>
#include <vector>

using namespace pd;

namespace
{

    int failures = 0;

    void Expect(const char *test, bool passed)
    {
        if (passed)
            return;
        std::printf("FAILED %s\n", test);
        ++failures;
    }

    // A mailbox in plain memory, holding one frame of a single luma value (chroma is U = 10, V = 20).
    class FakeMailbox
    {
    public:
        FakeMailbox() : memory_(kCamMapBytes) { shared_ = reinterpret_cast<CamShared *>(memory_.data()); }
        CamShared *Shared() { return shared_; }

        void Write(int width, int height, BYTE luma)
        {
            BYTE *pixels = CamPixels(shared_);
            const size_t lumaBytes = size_t(width) * height;
            memset(pixels, luma, lumaBytes);
            for (size_t i = 0; i < lumaBytes / 2; i += 2)
            {
                pixels[lumaBytes + i] = 10;
                pixels[lumaBytes + i + 1] = 20;
            }
            shared_->width = width;
            shared_->height = height;
            shared_->seq = shared_->seq + 2;
        }

    private:
        std::vector<BYTE> memory_;
        CamShared *shared_;
    };

    std::vector<BYTE> BlitTo(const FrameCopy &copy, UINT32 w, UINT32 h)
    {
        std::vector<BYTE> out(size_t(w) * h * 3 / 2);
        copy.Blit(out.data(), LONG(w), w, h);
        return out;
    }

    void AFrameJustWrittenIsFresh()
    {
        CamShared shared{};
        shared.writeTick = 5000;
        Expect("same tick", FrameIsFresh(shared, 5000));
        Expect("as old as allowed", FrameIsFresh(shared, 5000 + kCamFrameStaleMs));
    }

    void AnOldFrameIsStale()
    {
        CamShared shared{};
        shared.writeTick = 5000;
        Expect("too old", !FrameIsFresh(shared, 5000 + kCamFrameStaleMs + 1));
    }

    void AFrameWrittenAfterTheClockWasReadIsFresh()
    {
        CamShared shared{};
        shared.writeTick = 5016;
        Expect("written after the clock read", FrameIsFresh(shared, 5000));
    }

    void NothingWrittenYetIsStale()
    {
        CamShared shared{};
        Expect("empty mailbox", !FrameIsFresh(shared, 60000));
    }

    void ACompleteFrameIsTaken()
    {
        FakeMailbox box;
        box.Write(16, 8, 77);
        FrameCopy copy;
        Expect("nothing before the first frame", !copy.HasFrame());
        copy.TakeNewest(box.Shared());
        Expect("has a frame", copy.HasFrame());
        const std::vector<BYTE> out = BlitTo(copy, 16, 8);
        Expect("luma copied", out[0] == 77 && out[16 * 8 - 1] == 77);
        Expect("chroma copied", out[16 * 8] == 10 && out[16 * 8 + 1] == 20);
    }

    void AFrameBeingWrittenKeepsThePreviousOne()
    {
        FakeMailbox box;
        box.Write(16, 8, 77);
        FrameCopy copy;
        copy.TakeNewest(box.Shared());
        box.Write(16, 8, 99);
        box.Shared()->seq = box.Shared()->seq + 1;
        copy.TakeNewest(box.Shared());
        Expect("previous frame stays", BlitTo(copy, 16, 8)[0] == 77);
    }

    void ImpossibleSizesAreRefused()
    {
        for (const auto [w, h] : {std::pair{15, 8}, std::pair{16, 7}, std::pair{0, 8}, std::pair{16, 0}, std::pair{1922, 8}, std::pair{16, 1082}, std::pair{-16, 8}})
        {
            FakeMailbox box;
            box.Write(16, 8, 77);
            box.Shared()->width = w;
            box.Shared()->height = h;
            FrameCopy copy;
            copy.TakeNewest(box.Shared());
            Expect("size refused", !copy.HasFrame());
        }
    }

    void ScalingKeepsUAndVTogether()
    {
        FakeMailbox box;
        box.Write(16, 8, 77);
        FrameCopy copy;
        copy.TakeNewest(box.Shared());
        for (const auto [w, h] : {std::pair{8u, 4u}, std::pair{32u, 16u}})
        {
            const std::vector<BYTE> out = BlitTo(copy, w, h);
            const size_t uv = size_t(w) * h;
            bool paired = true;
            for (size_t i = 0; i + 1 < size_t(w) * h / 2; i += 2)
                paired = paired && out[uv + i] == 10 && out[uv + i + 1] == 20;
            Expect("U and V in their places", paired);
            Expect("luma scaled", out[0] == 77 && out[uv - 1] == 77);
        }
    }

    void TheNoSignalBarMoves()
    {
        constexpr UINT32 w = 128, h = 72;
        std::vector<BYTE> first(size_t(w) * h * 3 / 2), later(first.size());
        DrawNoSignal(first.data(), LONG(w), w, h, 0);
        DrawNoSignal(later.data(), LONG(w), w, h, 800);
        Expect("the background is dark grey", first[0] == 0x28);
        Expect("no colour", first[size_t(w) * h] == 0x80);
        Expect("the picture changes", first != later);
    }

} // namespace

int main()
{
    AFrameJustWrittenIsFresh();
    AnOldFrameIsStale();
    AFrameWrittenAfterTheClockWasReadIsFresh();
    NothingWrittenYetIsStale();
    ACompleteFrameIsTaken();
    AFrameBeingWrittenKeepsThePreviousOne();
    ImpossibleSizesAreRefused();
    ScalingKeepsUAndVTogether();
    TheNoSignalBarMoves();
    if (failures == 0)
        std::puts("camframe: all tests passed");
    return failures == 0 ? 0 : 1;
}
