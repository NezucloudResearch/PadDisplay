// PadDisplay by Nezucloud. Copyright (c) 2026 Nezucloud.
// SPDX-License-Identifier: MIT
// Hardware video encoder interface. Implementations take B8G8R8A8 D3D11 textures on the
// capture device and produce Annex-B H.264/HEVC access units.
#pragma once
#include "protocol.h"

#include <d3d11.h>
#include <cstdint>
#include <memory>
#include <vector>

namespace pd {

struct EncoderConfig {
    int width = 0, height = 0, fps = 60;
    Codec codec = CodecHEVC;
    int bitrateKbps = 20000;
    int preset = 0; // NVENC P1..P7, 0 = auto
};

class Encoder {
public:
    virtual ~Encoder() = default;
    virtual bool Init(ID3D11Device* device, const EncoderConfig& cfg) = 0;
    // The encoder's persistent input image (B8G8R8A8, render-target bindable). Capture keeps it up
    // to date incrementally, so it is the same texture for every frame.
    virtual ID3D11Texture2D* InputTexture() = 0;
    // Encodes InputTexture(). Returns once the encoder no longer reads it (synchronous).
    virtual bool Encode(bool forceIdr, uint64_t ptsUs, std::vector<uint8_t>& out, bool& keyframe) = 0;
    // Codec parameter sets (VPS/SPS/PPS) in Annex-B.
    virtual std::vector<uint8_t> ParameterSets() = 0;
    virtual bool SetBitrate(int kbps) = 0;
    virtual const char* Name() const = 0;
};

std::unique_ptr<Encoder> CreateNvencEncoder();
std::unique_ptr<Encoder> CreateMfEncoder();

} // namespace pd
