// PadDisplay by Nezucloud. Copyright (c) 2026 Nezucloud.
// SPDX-License-Identifier: MIT
#include "encoder/gpu_select.h"

#include <cstdio>
#include <vector>

using namespace pd;

namespace
{

    int failures = 0;

    constexpr UINT kNvidia = 0x10DE, kIntel = 0x8086, kAmd = 0x1002;

    const EncodeGpu kIgpu{L"Intel UHD", kIntel, true};
    const EncodeGpu kRtx{L"NVIDIA RTX", kNvidia, false};
    const EncodeGpu kRadeon{L"AMD Radeon", kAmd, false};
    const GpuSituation kIdle{};
    const GpuSituation kGaming{true, false};
    const GpuSituation kOnBattery{false, true};

    void Expect(const char *test, const std::wstring &got, const EncodeGpu &want)
    {
        if (got == want.name)
            return;
        std::printf("FAILED %s: got %ls\n", test, got.c_str());
        ++failures;
    }

    std::wstring Chosen(const std::vector<EncodeGpu> &gpus, const wchar_t *setting, const GpuSituation &now)
    {
        return ChooseEncodeGpu(gpus, setting, now).gpu.name;
    }

    void AutoPrefersNvencOnMains()
    {
        Expect("idle", Chosen({kIgpu, kRtx}, L"auto", kIdle), kRtx);
    }

    void AutoMovesToTheIntegratedGpuForGamingOrBattery()
    {
        Expect("gaming", Chosen({kIgpu, kRtx}, L"auto", kGaming), kIgpu);
        Expect("battery", Chosen({kIgpu, kRtx}, L"auto", kOnBattery), kIgpu);
    }

    void AutoStaysOnNvidiaWithoutAnIntegratedGpu()
    {
        Expect("gaming", Chosen({kRtx}, L"auto", kGaming), kRtx);
        Expect("battery", Chosen({kRtx}, L"auto", kOnBattery), kRtx);
    }

    void AutoTakesTheIntegratedGpuForAnyOtherPair()
    {
        Expect("Radeon and Intel", Chosen({kRadeon, kIgpu}, L"auto", kIdle), kIgpu);
    }

    void AutoTakesTheOnlyGpu()
    {
        Expect("Radeon alone", Chosen({kRadeon}, L"auto", kIdle), kRadeon);
    }

    void ANamedGpuWinsInEverySituation()
    {
        Expect("idle", Chosen({kIgpu, kRtx}, L"Intel UHD", kIdle), kIgpu);
        Expect("gaming", Chosen({kIgpu, kRtx}, L"NVIDIA RTX", kGaming), kRtx);
        Expect("battery", Chosen({kIgpu, kRtx}, L"NVIDIA RTX", kOnBattery), kRtx);
    }

    void TheNameIsMatchedWithoutCase()
    {
        Expect("case", Chosen({kIgpu, kRtx}, L"intel uhd", kIdle), kIgpu);
    }

    void AGpuThatIsGoneFallsBackToAuto()
    {
        Expect("removed", Chosen({kIgpu, kRtx}, L"Old eGPU", kGaming), kIgpu);
    }

    void EveryChoiceGivesAReason()
    {
        for (const GpuSituation &now : {kIdle, kGaming, kOnBattery})
            if (!ChooseEncodeGpu({kIgpu, kRtx}, L"auto", now).why[0])
            {
                std::puts("FAILED a choice without a reason");
                ++failures;
            }
    }

} // namespace

int main()
{
    AutoPrefersNvencOnMains();
    AutoMovesToTheIntegratedGpuForGamingOrBattery();
    AutoStaysOnNvidiaWithoutAnIntegratedGpu();
    AutoTakesTheIntegratedGpuForAnyOtherPair();
    AutoTakesTheOnlyGpu();
    ANamedGpuWinsInEverySituation();
    TheNameIsMatchedWithoutCase();
    AGpuThatIsGoneFallsBackToAuto();
    EveryChoiceGivesAReason();
    if (failures == 0)
        std::puts("gpu_select: all tests passed");
    return failures == 0 ? 0 : 1;
}
