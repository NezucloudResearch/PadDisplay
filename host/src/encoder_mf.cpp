// PadDisplay by Nezucloud. Copyright (c) 2026 Nezucloud.
// SPDX-License-Identifier: MIT
// Media Foundation hardware encoder (Intel Quick Sync / AMD AMF MFTs). Used when the virtual
// display is rendered by a non-NVIDIA adapter. BGRA -> NV12 happens on the GPU with the
// D3D11 video processor, then the NV12 surface goes to the async hardware MFT.
#include "encoder.h"
#include "log.h"

#include <strmif.h>
#include <codecapi.h>
#include <mfapi.h>
#include <mferror.h>
#include <mftransform.h>
#include <wrl/client.h>
#include <array>

using Microsoft::WRL::ComPtr;

namespace pd {
namespace {

constexpr int kRing = 3;

class MfEncoder final : public Encoder {
public:
    ~MfEncoder() override {
        if (mft_) {
            mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
            mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
            mft_->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, 0);
        }
        if (activate_) activate_->ShutdownObject();
    }

    bool Init(ID3D11Device* device, const EncoderConfig& cfg) override {
        cfg_ = cfg;
        device_ = device;
        device_->GetImmediateContext(&ctx_);
        if (!FindMft()) return false;
        if (!InitConverter()) return false;

        ComPtr<IMFAttributes> attrs;
        mft_->GetAttributes(&attrs);
        UINT32 async = 0;
        if (attrs && SUCCEEDED(attrs->GetUINT32(MF_TRANSFORM_ASYNC, &async)) && async)
            attrs->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
        if (FAILED(mft_.As(&events_))) return Fail("MFT is not async");

        UINT token = 0;
        if (FAILED(MFCreateDXGIDeviceManager(&token, &dxgiManager_)) || FAILED(dxgiManager_->ResetDevice(device, token)))
            return Fail("DXGI device manager");
        if (FAILED(mft_->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, reinterpret_cast<ULONG_PTR>(dxgiManager_.Get()))))
            return Fail("MFT rejected D3D manager");

        mft_.As(&codecApi_);
        if (codecApi_) {
            SetCodecValue(CODECAPI_AVLowLatencyMode, VT_BOOL, 1);
            SetCodecValue(CODECAPI_AVEncCommonRateControlMode, VT_UI4, eAVEncCommonRateControlMode_CBR);
            SetCodecValue(CODECAPI_AVEncCommonMeanBitRate, VT_UI4, cfg.bitrateKbps * 1000);
            SetCodecValue(CODECAPI_AVEncMPVDefaultBPictureCount, VT_UI4, 0);
            SetCodecValue(CODECAPI_AVEncMPVGOPSize, VT_UI4, 0xFFFFFFFF);
            SetCodecValue(CODECAPI_AVEncCommonQualityVsSpeed, VT_UI4, 60);
        }

        ComPtr<IMFMediaType> out, in;
        MFCreateMediaType(&out);
        out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        out->SetGUID(MF_MT_SUBTYPE, cfg.codec == CodecHEVC ? MFVideoFormat_HEVC : MFVideoFormat_H264);
        out->SetUINT32(MF_MT_AVG_BITRATE, cfg.bitrateKbps * 1000);
        MFSetAttributeSize(out.Get(), MF_MT_FRAME_SIZE, cfg.width, cfg.height);
        MFSetAttributeRatio(out.Get(), MF_MT_FRAME_RATE, cfg.fps, 1);
        out->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        out->SetUINT32(MF_MT_MPEG2_PROFILE, cfg.codec == CodecHEVC ? eAVEncH265VProfile_Main_420_8 : eAVEncH264VProfile_High);
        if (FAILED(mft_->SetOutputType(0, out.Get(), 0))) return Fail("SetOutputType");

        MFCreateMediaType(&in);
        in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        in->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
        MFSetAttributeSize(in.Get(), MF_MT_FRAME_SIZE, cfg.width, cfg.height);
        MFSetAttributeRatio(in.Get(), MF_MT_FRAME_RATE, cfg.fps, 1);
        in->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        if (FAILED(mft_->SetInputType(0, in.Get(), 0))) return Fail("SetInputType");

        MFT_OUTPUT_STREAM_INFO si{};
        mft_->GetOutputStreamInfo(0, &si);
        providesSamples_ = (si.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
        outBufferSize_ = std::max<DWORD>(si.cbSize, DWORD(cfg.width * cfg.height));

        mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
        mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
        LOGI("mf: %s %s %dx%d@%d %d kbps", name_.c_str(), cfg.codec == CodecHEVC ? "HEVC" : "H.264", cfg.width, cfg.height,
             cfg.fps, cfg.bitrateKbps);
        return true;
    }

    ID3D11Texture2D* InputTexture() override { return bgra_[0].Get(); }

    bool Encode(bool forceIdr, uint64_t ptsUs, std::vector<uint8_t>& out, bool& keyframe) override {
        cur_ = (cur_ + 1) % kRing; // NV12 ring: the async MFT may still hold the previous surface
        D3D11_VIDEO_PROCESSOR_STREAM stream{};
        stream.Enable = TRUE;
        stream.pInputSurface = inViews_[0].Get();
        if (FAILED(videoCtx_->VideoProcessorBlt(processor_.Get(), outViews_[cur_].Get(), 0, 1, &stream))) return Fail("VideoProcessorBlt");

        ComPtr<IMFMediaBuffer> buf;
        ComPtr<IMFSample> sample;
        if (FAILED(MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D), nv12_[cur_].Get(), 0, FALSE, &buf))) return Fail("surface buffer");
        MFCreateSample(&sample);
        sample->AddBuffer(buf.Get());
        sample->SetSampleTime(LONGLONG(ptsUs) * 10);
        sample->SetSampleDuration(10000000LL / cfg_.fps);

        if (forceIdr && codecApi_) SetCodecValue(CODECAPI_AVEncVideoForceKeyFrame, VT_UI4, 1);

        while (needInput_ == 0)
            if (!PumpEvent()) return false;
        if (FAILED(mft_->ProcessInput(0, sample.Get(), 0))) return Fail("ProcessInput");
        --needInput_;

        haveOutput_ = false;
        while (!haveOutput_)
            if (!PumpEvent()) return false;
        out.swap(lastOutput_);
        keyframe = lastKey_;
        return !out.empty();
    }

    std::vector<uint8_t> ParameterSets() override { return {}; } // in-band with each IDR

    bool SetBitrate(int kbps) override {
        cfg_.bitrateKbps = kbps;
        return codecApi_ && SetCodecValue(CODECAPI_AVEncCommonMeanBitRate, VT_UI4, kbps * 1000);
    }

    const char* Name() const override { return "MediaFoundation"; }

private:
    bool FindMft() {
        ComPtr<IDXGIDevice> dxgiDev;
        ComPtr<IDXGIAdapter> adapter;
        DXGI_ADAPTER_DESC ad{};
        if (SUCCEEDED(device_.As(&dxgiDev)) && SUCCEEDED(dxgiDev->GetAdapter(&adapter))) adapter->GetDesc(&ad);

        MFT_REGISTER_TYPE_INFO outType{MFMediaType_Video, cfg_.codec == CodecHEVC ? MFVideoFormat_HEVC : MFVideoFormat_H264};
        IMFActivate** acts = nullptr;
        UINT32 count = 0;
        if (FAILED(MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER, nullptr,
                             &outType, &acts, &count)) || count == 0)
            return Fail("no hardware encoder MFT for this codec");
        int chosen = -1;
        for (UINT32 i = 0; i < count && chosen < 0; ++i) {
            UINT64 luid = 0;
            if (SUCCEEDED(acts[i]->GetUINT64(MFT_ENUM_ADAPTER_LUID, &luid)) &&
                luid == ((UINT64(UINT32(ad.AdapterLuid.HighPart)) << 32) | ad.AdapterLuid.LowPart))
                chosen = int(i);
        }
        if (chosen < 0) chosen = 0;
        wchar_t name[256] = L"?";
        acts[chosen]->GetString(MFT_FRIENDLY_NAME_Attribute, name, 256, nullptr);
        name_ = Narrow(name);
        HRESULT hr = acts[chosen]->ActivateObject(IID_PPV_ARGS(&mft_));
        activate_ = acts[chosen];
        for (UINT32 i = 0; i < count; ++i) acts[i]->Release();
        CoTaskMemFree(acts);
        if (FAILED(hr)) return Fail("ActivateObject");
        return true;
    }

    bool InitConverter() {
        if (FAILED(device_.As(&videoDev_)) || FAILED(ctx_.As(&videoCtx_))) return Fail("no D3D11 video device");
        D3D11_VIDEO_PROCESSOR_CONTENT_DESC cd{};
        cd.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
        cd.InputFrameRate = {UINT(cfg_.fps), 1};
        cd.InputWidth = cd.OutputWidth = cfg_.width;
        cd.InputHeight = cd.OutputHeight = cfg_.height;
        cd.OutputFrameRate = {UINT(cfg_.fps), 1};
        cd.Usage = D3D11_VIDEO_USAGE_OPTIMAL_SPEED;
        if (FAILED(videoDev_->CreateVideoProcessorEnumerator(&cd, &vpEnum_)) ||
            FAILED(videoDev_->CreateVideoProcessor(vpEnum_.Get(), 0, &processor_)))
            return Fail("video processor");
        D3D11_VIDEO_PROCESSOR_COLOR_SPACE inCs{}, outCs{};
        inCs.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_0_255;
        outCs.YCbCr_Matrix = 1; // BT.709
        outCs.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
        videoCtx_->VideoProcessorSetStreamColorSpace(processor_.Get(), 0, &inCs);
        videoCtx_->VideoProcessorSetOutputColorSpace(processor_.Get(), &outCs);
        videoCtx_->VideoProcessorSetStreamAutoProcessingMode(processor_.Get(), 0, FALSE);

        for (int i = 0; i < kRing; ++i) {
            D3D11_TEXTURE2D_DESC td{};
            td.Width = cfg_.width;
            td.Height = cfg_.height;
            td.MipLevels = td.ArraySize = 1;
            td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            td.SampleDesc.Count = 1;
            td.Usage = D3D11_USAGE_DEFAULT;
            td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
            if (i == 0 && FAILED(device_->CreateTexture2D(&td, nullptr, &bgra_[0]))) return Fail("BGRA texture");
            td.Format = DXGI_FORMAT_NV12;
            td.BindFlags = D3D11_BIND_RENDER_TARGET;
            if (FAILED(device_->CreateTexture2D(&td, nullptr, &nv12_[i]))) return Fail("NV12 texture");

            D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC iv{};
            iv.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
            D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ov{};
            ov.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
            if ((i == 0 && FAILED(videoDev_->CreateVideoProcessorInputView(bgra_[0].Get(), vpEnum_.Get(), &iv, &inViews_[0]))) ||
                FAILED(videoDev_->CreateVideoProcessorOutputView(nv12_[i].Get(), vpEnum_.Get(), &ov, &outViews_[i])))
                return Fail("video processor views");
        }
        return true;
    }

    // Handles one MFT event; collects output into lastOutput_.
    bool PumpEvent() {
        ComPtr<IMFMediaEvent> ev;
        if (FAILED(events_->GetEvent(0, &ev))) return Fail("GetEvent");
        MediaEventType type = MEUnknown;
        ev->GetType(&type);
        if (type == METransformNeedInput) {
            ++needInput_;
        } else if (type == METransformHaveOutput) {
            MFT_OUTPUT_DATA_BUFFER odb{};
            ComPtr<IMFSample> own;
            if (!providesSamples_) {
                ComPtr<IMFMediaBuffer> mb;
                MFCreateMemoryBuffer(outBufferSize_, &mb);
                MFCreateSample(&own);
                own->AddBuffer(mb.Get());
                odb.pSample = own.Get();
            }
            DWORD status = 0;
            HRESULT hr = mft_->ProcessOutput(0, 1, &odb, &status);
            if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
                // Re-negotiate the (unchanged) output type and carry on.
                ComPtr<IMFMediaType> t;
                if (SUCCEEDED(mft_->GetOutputAvailableType(0, 0, &t))) mft_->SetOutputType(0, t.Get(), 0);
                if (odb.pEvents) odb.pEvents->Release();
                return true;
            }
            if (FAILED(hr)) return Fail("ProcessOutput");
            IMFSample* s = odb.pSample;
            ComPtr<IMFMediaBuffer> contiguous;
            if (s && SUCCEEDED(s->ConvertToContiguousBuffer(&contiguous))) {
                BYTE* p = nullptr;
                DWORD len = 0;
                contiguous->Lock(&p, nullptr, &len);
                lastOutput_.assign(p, p + len);
                contiguous->Unlock();
                lastKey_ = MFGetAttributeUINT32(s, MFSampleExtension_CleanPoint, 0) != 0;
            }
            if (providesSamples_ && odb.pSample) odb.pSample->Release();
            if (odb.pEvents) odb.pEvents->Release();
            haveOutput_ = true;
        }
        return true;
    }

    bool SetCodecValue(const GUID& key, VARTYPE vt, ULONG value) {
        VARIANT v;
        VariantInit(&v);
        v.vt = vt;
        if (vt == VT_BOOL) v.boolVal = value ? VARIANT_TRUE : VARIANT_FALSE;
        else v.ulVal = value;
        return SUCCEEDED(codecApi_->SetValue(&key, &v));
    }

    bool Fail(const char* what) {
        LOGI("mf: %s", what);
        return false;
    }

    EncoderConfig cfg_;
    std::string name_;
    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> ctx_;
    ComPtr<ID3D11VideoDevice> videoDev_;
    ComPtr<ID3D11VideoContext> videoCtx_;
    ComPtr<ID3D11VideoProcessorEnumerator> vpEnum_;
    ComPtr<ID3D11VideoProcessor> processor_;
    std::array<ComPtr<ID3D11Texture2D>, kRing> bgra_, nv12_;
    std::array<ComPtr<ID3D11VideoProcessorInputView>, kRing> inViews_;
    std::array<ComPtr<ID3D11VideoProcessorOutputView>, kRing> outViews_;
    int cur_ = 0;

    ComPtr<IMFActivate> activate_;
    ComPtr<IMFTransform> mft_;
    ComPtr<IMFMediaEventGenerator> events_;
    ComPtr<IMFDXGIDeviceManager> dxgiManager_;
    ComPtr<ICodecAPI> codecApi_;
    bool providesSamples_ = false;
    DWORD outBufferSize_ = 0;
    int needInput_ = 0;
    bool haveOutput_ = false;
    std::vector<uint8_t> lastOutput_;
    bool lastKey_ = false;
};

} // namespace

std::unique_ptr<Encoder> CreateMfEncoder() { return std::make_unique<MfEncoder>(); }

} // namespace pd
