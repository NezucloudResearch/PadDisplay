// PadDisplay by Nezucloud. Copyright (c) 2026 Nezucloud.
// SPDX-License-Identifier: MIT
// Which GPU renders the virtual monitor, and so which hardware encoder streams it (NVENC on NVIDIA, Quick Sync on Intel,
// AMF on AMD): the encoder has to sit on the GPU that draws the monitor, or every frame would cross the PCIe bus.
#pragma once
#include <windows.h>
#include <string>
#include <vector>

namespace pd {

struct EncodeGpu {
    std::wstring name;    // DXGI description; this is also what the virtual monitor driver is told ("friendlyname")
    UINT vendorId = 0;
    bool integrated = false;
};

// The hardware GPUs of this PC, one entry per name (an NVIDIA laptop GPU shows up as two adapters).
std::vector<EncodeGpu> ListEncodeGpus();

// "Quick Sync", "NVENC" or "AMF": what encodes on a GPU of this vendor.
const wchar_t* EncoderName(UINT vendorId);

struct GpuSituation {
    bool gaming = false;    // the tablet asked for gaming mode: a game is expected to keep the main GPU busy
    bool onBattery = false;
};

// settings.ini "gpu": "auto", or the name of one GPU: that one is used, in every mode (extra screen or only screen), whatever else is
// going on. gpus is not empty. why gets a short reason for the log. Only "auto" decides by itself:
//
// Auto, from measurements on a laptop with an Intel UHD (Quick Sync) and an RTX 4050 (NVENC), 2560x1440 at 120 Hz:
//  - with a game running on the NVIDIA GPU, NVENC fell to 42-49 fps (frames of 19-22 ms) and slowed the game by 8%; Quick Sync
//    held 114-118 fps (7-8 ms) and the game lost under 2%;
//  - with no game, NVENC took 5.4 ms a frame and a quarter of the CPU time Quick Sync needed (7.8 ms);
//  - a discrete GPU that is asleep costs several watts to wake.
// So: gaming or on battery, use the integrated GPU; otherwise NVIDIA's NVENC. Any other combination (AMD, Intel Arc) was not
// measured, and takes the integrated GPU.
const EncodeGpu& ChooseEncodeGpu(const std::vector<EncodeGpu>& gpus, const std::wstring& setting, const GpuSituation& now,
                                 std::wstring& why);

bool OnBattery();

} // namespace pd
