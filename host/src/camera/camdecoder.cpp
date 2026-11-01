// PadDisplay by Nezucloud. Copyright (c) 2026 Nezucloud.
// SPDX-License-Identifier: MIT
#include "camdecoder.h"

#include <codecapi.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <wmcodecdsp.h>
#include <algorithm>
#include <cstring>
#include <utility>

using Microsoft::WRL::ComPtr;

namespace pd
{
    namespace
    {

        constexpr int64_t kSampleTimePerUs = 10; // Media Foundation counts in 100 ns

        // MF_LOW_LATENCY is the decoder's own switch for "give me each frame as soon as it is decoded"; without it
        // the decoder holds several frames back before outputting any.
        void AskForLowLatency(const ComPtr<IMFTransform> &mft)
        {
            ComPtr<IMFAttributes> attrs;
            if (SUCCEEDED(mft->GetAttributes(&attrs)))
                attrs->SetUINT32(MF_LOW_LATENCY, TRUE);
            ComPtr<ICodecAPI> api;
            if (FAILED(mft.As(&api)))
                return;
            VARIANT v;
            VariantInit(&v);
            v.vt = VT_BOOL;
            v.boolVal = VARIANT_TRUE;
            api->SetValue(&CODECAPI_AVLowLatencyMode, &v);
        }

    } // namespace

    CamDecoder::MediaFoundation::MediaFoundation() : started(SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE))) {}

    CamDecoder::MediaFoundation::~MediaFoundation()
    {
        if (started)
            MFShutdown();
    }

    CamDecoder::CamDecoder(FrameFn onFrame) : onFrame_(std::move(onFrame)) {}

    bool CamDecoder::Init(uint32_t width, uint32_t height, uint32_t fps)
    {
        if (!mediaFoundation_.started)
            return false;
        if (FAILED(CoCreateInstance(CLSID_CMSH264DecoderMFT, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&mft_))))
            return false;
        AskForLowLatency(mft_);
        if (!SetInput(width, height, fps) || !ChooseOutput())
            return false;
        mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
        mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
        return true;
    }

    bool CamDecoder::SetInput(uint32_t width, uint32_t height, uint32_t fps)
    {
        ComPtr<IMFMediaType> type;
        if (FAILED(MFCreateMediaType(&type)))
            return false;
        type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
        type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        MFSetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, width, height);
        MFSetAttributeRatio(type.Get(), MF_MT_FRAME_RATE, fps, 1);
        return SUCCEEDED(mft_->SetInputType(0, type.Get(), 0));
    }

    bool CamDecoder::ChooseOutput()
    {
        for (DWORD i = 0;; ++i)
        {
            ComPtr<IMFMediaType> type;
            if (FAILED(mft_->GetOutputAvailableType(0, i, &type)))
                return false;
            GUID subtype{};
            type->GetGUID(MF_MT_SUBTYPE, &subtype);
            if (subtype != MFVideoFormat_NV12)
                continue;
            if (FAILED(mft_->SetOutputType(0, type.Get(), 0)))
                return false;
            ReadLayout(type.Get());
            return true;
        }
    }

    void CamDecoder::ReadLayout(IMFMediaType *type)
    {
        UINT32 frameWidth = 0, frameHeight = 0;
        MFGetAttributeSize(type, MF_MT_FRAME_SIZE, &frameWidth, &frameHeight);
        width_ = frameWidth;
        height_ = frameHeight;
        MFVideoArea area{};
        if (SUCCEEDED(type->GetBlob(MF_MT_MINIMUM_DISPLAY_APERTURE, reinterpret_cast<UINT8 *>(&area), sizeof(area), nullptr)) && area.Area.cx > 0 && area.Area.cy > 0)
        {
            width_ = UINT32(area.Area.cx);
            height_ = UINT32(area.Area.cy);
        }
        width_ &= ~1u; // NV12 chroma is subsampled by two both ways
        height_ &= ~1u;
        LONG stride = 0;
        if (FAILED(MFGetStrideForBitmapInfoHeader(MFVideoFormat_NV12.Data1, frameWidth, &stride)) || stride <= 0)
            stride = LONG(frameWidth);
        stride_ = UINT32(stride);
        UINT32 declared = 0;
        if (SUCCEEDED(type->GetUINT32(MF_MT_DEFAULT_STRIDE, &declared)) && declared > 0)
            stride_ = declared;
    }

    void CamDecoder::Flush()
    {
        mft_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
    }

    bool CamDecoder::Decode(const uint8_t *data, size_t size, int64_t ptsUs)
    {
        ptsUs_ = ptsUs;
        return Feed(data, size) && Drain();
    }

    // A new sample per unit: the decoder may keep the buffer. The output is drained after every unit, so a decoder that still
    // refuses input (MF_E_NOTACCEPTING) is an error like any other: losing the unit quietly would smear every frame after it.
    bool CamDecoder::Feed(const uint8_t *data, size_t size)
    {
        ComPtr<IMFSample> sample;
        ComPtr<IMFMediaBuffer> buffer;
        if (FAILED(MFCreateSample(&sample)) || FAILED(MFCreateMemoryBuffer(DWORD(size), &buffer)))
            return false;
        BYTE *to = nullptr;
        if (FAILED(buffer->Lock(&to, nullptr, nullptr)))
            return false;
        memcpy(to, data, size);
        buffer->Unlock();
        buffer->SetCurrentLength(DWORD(size));
        sample->AddBuffer(buffer.Get());
        sample->SetSampleTime(ptsUs_ * kSampleTimePerUs);
        return SUCCEEDED(mft_->ProcessInput(0, sample.Get(), 0));
    }

    bool CamDecoder::Drain()
    {
        for (;;)
        {
            MFT_OUTPUT_STREAM_INFO info{};
            mft_->GetOutputStreamInfo(0, &info);
            const bool decoderAllocates = (info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
            if (!decoderAllocates && !PrepareOutputSample(info.cbSize))
                return false;
            MFT_OUTPUT_DATA_BUFFER out{};
            out.pSample = decoderAllocates ? nullptr : outputSample_.Get();
            DWORD status = 0;
            const HRESULT hr = mft_->ProcessOutput(0, 1, &out, &status);
            // Ours to release whatever the result.
            ComPtr<IMFCollection> events;
            events.Attach(out.pEvents);
            ComPtr<IMFSample> allocated;
            if (decoderAllocates)
                allocated.Attach(out.pSample);

            if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT)
                return true;
            if (hr == MF_E_TRANSFORM_STREAM_CHANGE)
            { // the stream's size changed
                if (!ChooseOutput())
                    return false;
                continue;
            }
            if (FAILED(hr))
                return false;
            Deliver(out.pSample);
        }
    }

    bool CamDecoder::PrepareOutputSample(DWORD size)
    {
        if (!outputSample_ || outputSize_ < size)
        {
            ComPtr<IMFSample> sample;
            ComPtr<IMFMediaBuffer> created;
            if (FAILED(MFCreateSample(&sample)) || FAILED(MFCreateMemoryBuffer(std::max<DWORD>(size, 1), &created)))
                return false;
            sample->AddBuffer(created.Get());
            outputSample_ = sample;
            outputSize_ = size;
        }
        ComPtr<IMFMediaBuffer> buffer;
        if (FAILED(outputSample_->GetBufferByIndex(0, &buffer)))
            return false;
        buffer->SetCurrentLength(0);
        return true;
    }

    void CamDecoder::Deliver(IMFSample *sample)
    {
        ComPtr<IMFMediaBuffer> buffer;
        if (!sample || FAILED(sample->ConvertToContiguousBuffer(&buffer)))
            return;
        BYTE *data = nullptr;
        DWORD length = 0;
        if (FAILED(buffer->Lock(&data, nullptr, &length)))
            return;
        // The buffer's rows are padded and its height is rounded up (1080 -> 1088); the UV plane starts after the padded Y plane.
        const uint32_t alignedHeight = stride_ ? length * 2 / 3 / stride_ : 0;
        if (alignedHeight >= height_)
            onFrame_({data, data + size_t(stride_) * alignedHeight, stride_, width_, height_, ptsUs_});
        buffer->Unlock();
    }

} // namespace pd
