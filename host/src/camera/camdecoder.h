// PadDisplay by Nezucloud. Copyright (c) 2026 Nezucloud.
// SPDX-License-Identifier: MIT
// The Media Foundation H.264 decoder in low-latency mode (it outputs a frame as soon as it can, with no reorder delay:
// the tablet sends no B-frames). It decodes in software: 720p/1080p30 costs a few percent of one core, and a hardware
// decoder would need a Direct3D device manager shared with the encoder's device for no visible gain at this size.
#pragma once
#include "cammailbox.h"
#include "comcall.h"

#include <mfobjects.h>
#include <mftransform.h>
#include <wrl/client.h>
#include <cstdint>
#include <functional>

namespace pd
{

    // Lives and dies on one thread, which it keeps in COM and Media Foundation for that long.
    class CamDecoder
    {
    public:
        using FrameFn = std::function<void(const Nv12Frame &)>;

        explicit CamDecoder(FrameFn onFrame);
        bool Init(uint32_t width, uint32_t height, uint32_t fps);
        void Flush();
        // Decodes one Annex-B access unit (or the SPS/PPS), calling onFrame for every frame that comes out. false = a decoder error.
        bool Decode(const uint8_t *data, size_t size, int64_t ptsUs);

    private:
        struct MediaFoundation
        {
            const bool started;
            MediaFoundation();
            ~MediaFoundation();
        };

        bool SetInput(uint32_t width, uint32_t height, uint32_t fps);
        bool ChooseOutput();
        void ReadLayout(IMFMediaType *type);
        bool Feed(const uint8_t *data, size_t size);
        bool Drain();
        bool PrepareOutputSample(DWORD size);
        void Deliver(IMFSample *sample);

        FrameFn onFrame_;
        // In this order: the decoder's objects must go while COM still runs. CoUninitialize can unload the decoder's DLL, and a
        // Release after that jumps into nothing.
        ComForThisCall com_;
        MediaFoundation mediaFoundation_;
        Microsoft::WRL::ComPtr<IMFTransform> mft_;
        Microsoft::WRL::ComPtr<IMFSample> outputSample_;
        DWORD outputSize_ = 0;
        uint32_t width_ = 0, height_ = 0, stride_ = 0;
        int64_t ptsUs_ = 0; // of the unit being decoded: the first frame after a CamConfig comes out with the wrong sample time
    };

} // namespace pd
