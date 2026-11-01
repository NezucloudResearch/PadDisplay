// PadDisplay by Nezucloud. Copyright (c) 2026 Nezucloud.
// SPDX-License-Identifier: MIT
#include "session/parameter_sets.h"

#include <cstdio>
#include <initializer_list>
#include <vector>

using namespace pd;

namespace
{

    int failures = 0;

    using Bytes = std::vector<uint8_t>;

    constexpr uint8_t kH264Sps = 0x67, kH264Pps = 0x68, kH264Idr = 0x65;
    constexpr uint8_t kHevcVps = 32 << 1, kHevcSps = 33 << 1, kHevcPps = 34 << 1, kHevcIdr = 19 << 1;

    void Expect(const char *test, bool passed)
    {
        if (passed)
            return;
        std::printf("FAILED %s\n", test);
        ++failures;
    }

    // A NAL unit behind a long or a short start code.
    Bytes Nal(uint8_t header, std::initializer_list<uint8_t> body, bool longStart = true)
    {
        Bytes out = longStart ? Bytes{0, 0, 0, 1} : Bytes{0, 0, 1};
        out.push_back(header);
        out.insert(out.end(), body);
        return out;
    }

    Bytes Join(std::initializer_list<Bytes> parts)
    {
        Bytes out;
        for (const Bytes &part : parts)
            out.insert(out.end(), part.begin(), part.end());
        return out;
    }

    void H264SetsAreCollected()
    {
        const Bytes au = Join({Nal(kH264Sps, {1, 2}), Nal(kH264Pps, {3}), Nal(kH264Idr, {9, 9, 9})});
        Expect("SPS and PPS, not the picture", ExtractParameterSets(au, CodecH264) == Join({Nal(kH264Sps, {1, 2}), Nal(kH264Pps, {3})}));
    }

    void HevcSetsAreCollected()
    {
        const Bytes au = Join({Nal(kHevcVps, {0, 1}), Nal(kHevcSps, {0, 2}), Nal(kHevcPps, {0, 3}), Nal(kHevcIdr, {0, 9})});
        Expect("VPS, SPS and PPS, not the picture", ExtractParameterSets(au, CodecHEVC) == Join({Nal(kHevcVps, {0, 1}), Nal(kHevcSps, {0, 2}), Nal(kHevcPps, {0, 3})}));
    }

    void ShortStartCodesBecomeLongOnes()
    {
        const Bytes au = Join({Nal(kH264Sps, {1}, false), Nal(kH264Idr, {2}, false)});
        Expect("long start code out", ExtractParameterSets(au, CodecH264) == Nal(kH264Sps, {1}));
    }

    void ASetAtTheVeryEndIsKept()
    {
        const Bytes au = Join({Nal(kH264Idr, {2}), Nal(kH264Pps, {7, 7})});
        Expect("runs to the end of the unit", ExtractParameterSets(au, CodecH264) == Nal(kH264Pps, {7, 7}));
    }

    void NothingToCollectGivesNothing()
    {
        Expect("empty unit", ExtractParameterSets({}, CodecH264).empty());
        Expect("no start code", ExtractParameterSets({1, 2, 3, 4}, CodecH264).empty());
        Expect("only a picture", ExtractParameterSets(Nal(kH264Idr, {1}), CodecH264).empty());
        Expect("a start code with nothing after it", ExtractParameterSets({0, 0, 0, 1}, CodecH264).empty());
    }

    void TheCodecDecidesHowAHeaderIsRead()
    {
        Expect("an HEVC VPS is no H.264 set", ExtractParameterSets(Nal(kHevcVps, {0}), CodecH264).empty());
        Expect("an H.264 SPS is no HEVC set", ExtractParameterSets(Nal(kH264Sps, {0}), CodecHEVC).empty());
    }

} // namespace

int main()
{
    H264SetsAreCollected();
    HevcSetsAreCollected();
    ShortStartCodesBecomeLongOnes();
    ASetAtTheVeryEndIsKept();
    NothingToCollectGivesNothing();
    TheCodecDecidesHowAHeaderIsRead();
    if (failures == 0)
        std::puts("parameter_sets: all tests passed");
    return failures == 0 ? 0 : 1;
}
