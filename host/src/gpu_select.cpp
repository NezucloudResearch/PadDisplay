// PadDisplay by Nezucloud. Copyright (c) 2026 Nezucloud.
// SPDX-License-Identifier: MIT
#include "gpu_select.h"

#include <dxgi.h>
#include <wrl/client.h>
#include <algorithm>

namespace pd {
namespace {

constexpr UINT kVendorNvidia = 0x10DE, kVendorIntel = 0x8086, kVendorAmd = 0x1002, kVendorMicrosoft = 0x1414;
constexpr SIZE_T kIntegratedMaxVram = SIZE_T(2) << 30; // an iGPU's dedicated memory is a BIOS carve-out; discrete cards start higher

// NVIDIA has no integrated GPUs on Windows; for the others a small dedicated memory means shared system memory.
bool LooksIntegrated(const DXGI_ADAPTER_DESC1& d) {
    return d.VendorId != kVendorNvidia && d.DedicatedVideoMemory < kIntegratedMaxVram;
}

const EncodeGpu* FindBy(const std::vector<EncodeGpu>& gpus, bool (*match)(const EncodeGpu&)) {
    auto it = std::find_if(gpus.begin(), gpus.end(), match);
    return it == gpus.end() ? nullptr : &*it;
}

} // namespace

std::vector<EncodeGpu> ListEncodeGpus() {
    std::vector<EncodeGpu> gpus;
    Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return gpus;
    Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 d;
        adapter->GetDesc1(&d);
        if ((d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) || d.VendorId == kVendorMicrosoft) continue;
        if (d.VendorId != kVendorNvidia && d.VendorId != kVendorIntel && d.VendorId != kVendorAmd) continue; // no encoder PadDisplay uses
        const bool known = std::any_of(gpus.begin(), gpus.end(), [&](const EncodeGpu& g) { return g.name == d.Description; });
        if (!known) gpus.push_back({d.Description, d.VendorId, LooksIntegrated(d)});
    }
    // DXGI lists the GPU this program prefers first, so the order changes with the preference: keep the tray menu's stable.
    std::stable_sort(gpus.begin(), gpus.end(), [](const EncodeGpu& a, const EncodeGpu& b) {
        return a.integrated != b.integrated ? a.integrated : a.name < b.name;
    });
    return gpus;
}

const wchar_t* EncoderName(UINT vendorId) {
    return vendorId == kVendorNvidia ? L"NVENC" : vendorId == kVendorIntel ? L"Quick Sync" : L"AMF";
}

const EncodeGpu& ChooseEncodeGpu(const std::vector<EncodeGpu>& gpus, const std::wstring& setting, const GpuSituation& now,
                                 std::wstring& why) {
    if (setting != L"auto") {
        auto named = std::find_if(gpus.begin(), gpus.end(), [&](const EncodeGpu& g) { return _wcsicmp(g.name.c_str(), setting.c_str()) == 0; });
        if (named != gpus.end()) {
            why = L"chosen in the tray menu";
            return *named;
        }
        // Chosen once, then removed or renamed (a driver update, an eGPU): do not strand the user on a GPU that is not there.
    }
    const EncodeGpu* integrated = FindBy(gpus, [](const EncodeGpu& g) { return g.integrated; });
    const EncodeGpu* nvidia = FindBy(gpus, [](const EncodeGpu& g) { return g.vendorId == kVendorNvidia; });
    if (integrated && (now.gaming || now.onBattery)) {
        why = now.gaming ? L"gaming mode: keeps the encoder off the game's GPU" : L"on battery: does not wake the discrete GPU";
        return *integrated;
    }
    if (nvidia) {
        why = L"NVENC encodes fastest and needs the least CPU";
        return *nvidia;
    }
    why = integrated ? L"the integrated GPU" : L"the first GPU";
    return integrated ? *integrated : gpus.front();
}

bool OnBattery() {
    SYSTEM_POWER_STATUS s{};
    return GetSystemPowerStatus(&s) && s.ACLineStatus == 0;
}

} // namespace pd
