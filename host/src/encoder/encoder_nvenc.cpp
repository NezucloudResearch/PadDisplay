// PadDisplay by Nezucloud. Copyright (c) 2026 Nezucloud.
// SPDX-License-Identifier: MIT
// NVIDIA NVENC encoder. D3D11 textures are registered directly with the encoder, so frames never
// leave VRAM; NVENC converts ARGB -> NV12 internally. Synchronous encode: one input texture, updated in place.
#include "encoder.h"
#include "log.h"
#include "realtime.h"

#include <ffnvcodec/nvEncodeAPI.h>
#include <wrl/client.h>
#include <algorithm>

using Microsoft::WRL::ComPtr;

namespace pd
{
    namespace
    {

        typedef NVENCSTATUS(NVENCAPI *PfnCreateInstance)(NV_ENCODE_API_FUNCTION_LIST *);
        typedef NVENCSTATUS(NVENCAPI *PfnGetMaxVersion)(uint32_t *);

        constexpr uint32_t kTimingLogEveryFrames = 1800;

        class NvencEncoder final : public Encoder
        {
        public:
            ~NvencEncoder() override { Shutdown(); }

            bool Init(ID3D11Device *device, const EncoderConfig &cfg) override
            {
                cfg_ = cfg;
                if (!LoadApi() || !OpenSession(device) || !Configure() || !InitializeEncoder() || !CreateInput(device))
                    return false;
                NV_ENC_CREATE_BITSTREAM_BUFFER bs{NV_ENC_CREATE_BITSTREAM_BUFFER_VER};
                if (Failed(fn_.nvEncCreateBitstreamBuffer(enc_, &bs), "bitstream buffer"))
                    return false;
                bitstream_ = bs.bitstreamBuffer;
                LOGI("nvenc: %s P%d %dx%d@%d %d kbps", cfg.codec == CodecHEVC ? "HEVC" : "H.264", preset_, cfg.width, cfg.height, cfg.fps, cfg.bitrateKbps);
                return true;
            }

            ID3D11Texture2D *InputTexture() override { return input_.Get(); }

            bool Encode(bool forceIdr, uint64_t ptsUs, std::vector<uint8_t> &out, bool &keyframe) override
            {
                uint64_t t0 = NowUs();
                NV_ENC_MAP_INPUT_RESOURCE map{NV_ENC_MAP_INPUT_RESOURCE_VER};
                map.registeredResource = registered_;
                if (Failed(fn_.nvEncMapInputResource(enc_, &map), "map"))
                    return false;

                NV_ENC_PIC_PARAMS pic{NV_ENC_PIC_PARAMS_VER};
                pic.inputWidth = cfg_.width;
                pic.inputHeight = cfg_.height;
                pic.inputPitch = cfg_.width;
                pic.inputBuffer = map.mappedResource;
                pic.bufferFmt = map.mappedBufferFmt;
                pic.outputBitstream = bitstream_;
                pic.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
                pic.inputTimeStamp = ptsUs;
                if (forceIdr)
                    pic.encodePicFlags = NV_ENC_PIC_FLAG_FORCEIDR | NV_ENC_PIC_FLAG_OUTPUT_SPSPPS;
                uint64_t t1 = NowUs();
                NVENCSTATUS st = fn_.nvEncEncodePicture(enc_, &pic);
                uint64_t t2 = NowUs();
                bool ok = st == NV_ENC_SUCCESS;
                if (ok)
                    ok = TakeBitstream(out, keyframe, t1 - t0, t2 - t1, t2);
                else
                    LOGI("nvenc: encode failed %d", st);
                fn_.nvEncUnmapInputResource(enc_, map.mappedResource);
                return ok;
            }

            std::vector<uint8_t> ParameterSets() override
            {
                std::vector<uint8_t> buf(1024);
                uint32_t size = 0;
                NV_ENC_SEQUENCE_PARAM_PAYLOAD sp{NV_ENC_SEQUENCE_PARAM_PAYLOAD_VER};
                sp.spsppsBuffer = buf.data();
                sp.inBufferSize = uint32_t(buf.size());
                sp.outSPSPPSPayloadSize = &size;
                if (Failed(fn_.nvEncGetSequenceParams(enc_, &sp), "sequence params"))
                    return {};
                buf.resize(size);
                return buf;
            }

            bool SetBitrate(int kbps) override
            {
                ApplyRate(kbps);
                NV_ENC_RECONFIGURE_PARAMS rc{NV_ENC_RECONFIGURE_PARAMS_VER};
                rc.reInitEncodeParams = init_;
                rc.reInitEncodeParams.encodeConfig = &config_;
                return !Failed(fn_.nvEncReconfigureEncoder(enc_, &rc), "reconfigure");
            }

            const char *Name() const override { return "NVENC"; }

        private:
            bool LoadApi()
            {
                dll_ = LoadLibraryW(L"nvEncodeAPI64.dll");
                if (!dll_)
                    return Fail("nvEncodeAPI64.dll not found (no NVIDIA driver?)");
                auto getMax = reinterpret_cast<PfnGetMaxVersion>(GetProcAddress(dll_, "NvEncodeAPIGetMaxSupportedVersion"));
                auto create = reinterpret_cast<PfnCreateInstance>(GetProcAddress(dll_, "NvEncodeAPICreateInstance"));
                if (!getMax || !create)
                    return Fail("NVENC entry points missing");
                uint32_t maxVer = 0;
                getMax(&maxVer);
                uint32_t need = (NVENCAPI_MAJOR_VERSION << 4) | NVENCAPI_MINOR_VERSION;
                if (maxVer < need)
                    return Fail("driver NVENC API too old (update the NVIDIA driver)");
                fn_.version = NV_ENCODE_API_FUNCTION_LIST_VER;
                return create(&fn_) == NV_ENC_SUCCESS || Fail("NvEncodeAPICreateInstance failed");
            }

            bool OpenSession(ID3D11Device *device)
            {
                NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS open{NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER};
                open.device = device;
                open.deviceType = NV_ENC_DEVICE_TYPE_DIRECTX;
                open.apiVersion = NVENCAPI_VERSION;
                return !Failed(fn_.nvEncOpenEncodeSessionEx(&open, &enc_), "open session");
            }

            GUID CodecGuid() const { return cfg_.codec == CodecHEVC ? NV_ENC_CODEC_HEVC_GUID : NV_ENC_CODEC_H264_GUID; }

            bool Configure()
            {
                const GUID presets[] = {NV_ENC_PRESET_P1_GUID, NV_ENC_PRESET_P2_GUID, NV_ENC_PRESET_P3_GUID, NV_ENC_PRESET_P4_GUID,
                                        NV_ENC_PRESET_P5_GUID, NV_ENC_PRESET_P6_GUID, NV_ENC_PRESET_P7_GUID};
                // Auto: H.264 P2 / HEVC P3. HEVC P1-P2 pad static frames up to the CBR budget.
                preset_ = cfg_.preset > 0 ? std::clamp(cfg_.preset, 1, 7) : (cfg_.codec == CodecHEVC ? 3 : 2);
                presetGuid_ = presets[preset_ - 1];
                NV_ENC_PRESET_CONFIG preset{NV_ENC_PRESET_CONFIG_VER};
                preset.presetCfg.version = NV_ENC_CONFIG_VER;
                if (Failed(fn_.nvEncGetEncodePresetConfigEx(enc_, CodecGuid(), presetGuid_, NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY, &preset), "preset"))
                    return false;
                config_ = preset.presetCfg;
                config_.version = NV_ENC_CONFIG_VER;
                config_.gopLength = NVENC_INFINITE_GOPLENGTH;
                config_.frameIntervalP = 1;
                ApplyRate(cfg_.bitrateKbps);
                if (cfg_.codec == CodecHEVC)
                    ConfigureHevc();
                else
                    ConfigureH264();
                return true;
            }

            void ConfigureHevc()
            {
                auto &h = config_.encodeCodecConfig.hevcConfig;
                config_.profileGUID = NV_ENC_HEVC_PROFILE_MAIN_GUID;
                h.idrPeriod = NVENC_INFINITE_GOPLENGTH;
                h.repeatSPSPPS = 1;
                h.chromaFormatIDC = 1;
                h.inputBitDepth = h.outputBitDepth = NV_ENC_BIT_DEPTH_8;
                SetVui(h.hevcVUIParameters);
            }

            void ConfigureH264()
            {
                auto &h = config_.encodeCodecConfig.h264Config;
                config_.profileGUID = NV_ENC_H264_PROFILE_HIGH_GUID;
                h.idrPeriod = NVENC_INFINITE_GOPLENGTH;
                h.repeatSPSPPS = 1;
                h.chromaFormatIDC = 1;
                SetVui(h.h264VUIParameters);
            }

            bool InitializeEncoder()
            {
                init_ = {NV_ENC_INITIALIZE_PARAMS_VER};
                init_.encodeGUID = CodecGuid();
                init_.presetGUID = presetGuid_;
                init_.tuningInfo = NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY;
                init_.encodeWidth = init_.darWidth = init_.maxEncodeWidth = cfg_.width;
                init_.encodeHeight = init_.darHeight = init_.maxEncodeHeight = cfg_.height;
                init_.frameRateNum = cfg_.fps;
                init_.frameRateDen = 1;
                init_.enablePTD = 1;
                init_.enableEncodeAsync = 0;
                init_.encodeConfig = &config_;
                return !Failed(fn_.nvEncInitializeEncoder(enc_, &init_), "initialize");
            }

            bool CreateInput(ID3D11Device *device)
            {
                D3D11_TEXTURE2D_DESC td{};
                td.Width = cfg_.width;
                td.Height = cfg_.height;
                td.MipLevels = td.ArraySize = 1;
                td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
                td.SampleDesc.Count = 1;
                td.Usage = D3D11_USAGE_DEFAULT;
                td.BindFlags = D3D11_BIND_RENDER_TARGET;
                if (FAILED(device->CreateTexture2D(&td, nullptr, &input_)))
                    return Fail("CreateTexture2D");
                NV_ENC_REGISTER_RESOURCE reg{NV_ENC_REGISTER_RESOURCE_VER};
                reg.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_DIRECTX;
                reg.resourceToRegister = input_.Get();
                reg.width = cfg_.width;
                reg.height = cfg_.height;
                reg.bufferFormat = NV_ENC_BUFFER_FORMAT_ARGB;
                reg.bufferUsage = NV_ENC_INPUT_IMAGE;
                if (Failed(fn_.nvEncRegisterResource(enc_, &reg), "register"))
                    return false;
                registered_ = reg.registeredResource;
                return true;
            }

            bool TakeBitstream(std::vector<uint8_t> &out, bool &keyframe, uint64_t mapUs, uint64_t submitUs, uint64_t submittedAt)
            {
                NV_ENC_LOCK_BITSTREAM lock{NV_ENC_LOCK_BITSTREAM_VER};
                lock.outputBitstream = bitstream_;
                if (Failed(fn_.nvEncLockBitstream(enc_, &lock), "lock"))
                    return false;
                auto *p = static_cast<const uint8_t *>(lock.bitstreamBufferPtr);
                out.assign(p, p + lock.bitstreamSizeInBytes);
                keyframe = lock.pictureType == NV_ENC_PIC_TYPE_IDR || lock.pictureType == NV_ENC_PIC_TYPE_I;
                fn_.nvEncUnlockBitstream(enc_, bitstream_);
                if (++timingFrames_ % kTimingLogEveryFrames == 0)
                    LOGI("nvenc timing: map %llu us, submit %llu us, wait+lock %llu us, %u bytes", mapUs, submitUs, NowUs() - submittedAt, lock.bitstreamSizeInBytes);
                return true;
            }

            void ApplyRate(int kbps)
            {
                auto &rc = config_.rcParams;
                rc.rateControlMode = NV_ENC_PARAMS_RC_CBR;
                rc.averageBitRate = rc.maxBitRate = uint32_t(kbps) * 1000;
                // Single-frame VBV: every frame fits in one frame interval on the wire.
                rc.vbvBufferSize = rc.vbvInitialDelay = uint32_t(kbps) * 1000 / cfg_.fps;
                rc.enableAQ = 1;
                rc.multiPass = NV_ENC_MULTI_PASS_DISABLED; // two-pass roughly doubles encode latency
            }

            static void SetVui(NV_ENC_CONFIG_H264_VUI_PARAMETERS &v)
            {
                v.videoSignalTypePresentFlag = 1;
                v.videoFormat = NV_ENC_VUI_VIDEO_FORMAT_UNSPECIFIED;
                v.videoFullRangeFlag = 0;
                v.colourDescriptionPresentFlag = 1;
                v.colourPrimaries = NV_ENC_VUI_COLOR_PRIMARIES_BT709;
                v.transferCharacteristics = NV_ENC_VUI_TRANSFER_CHARACTERISTIC_BT709;
                v.colourMatrix = NV_ENC_VUI_MATRIX_COEFFS_BT709;
            }

            bool Fail(const char *what)
            {
                LOGI("nvenc: %s", what);
                return false;
            }

            // Logs a call that went wrong. true = it did.
            bool Failed(NVENCSTATUS st, const char *what)
            {
                if (st == NV_ENC_SUCCESS)
                    return false;
                const char *detail = enc_ && fn_.nvEncGetLastErrorString ? fn_.nvEncGetLastErrorString(enc_) : "";
                LOGI("nvenc: %s failed (%d) %s", what, st, detail ? detail : "");
                return true;
            }

            void Shutdown()
            {
                if (enc_)
                {
                    if (registered_)
                        fn_.nvEncUnregisterResource(enc_, registered_);
                    if (bitstream_)
                        fn_.nvEncDestroyBitstreamBuffer(enc_, bitstream_);
                    fn_.nvEncDestroyEncoder(enc_);
                    enc_ = nullptr;
                }
                input_.Reset();
                if (dll_)
                    FreeLibrary(dll_);
                dll_ = nullptr;
            }

            EncoderConfig cfg_;
            HMODULE dll_ = nullptr;
            NV_ENCODE_API_FUNCTION_LIST fn_{};
            void *enc_ = nullptr;
            GUID presetGuid_{};
            int preset_ = 3;
            NV_ENC_CONFIG config_{};
            NV_ENC_INITIALIZE_PARAMS init_{};
            ComPtr<ID3D11Texture2D> input_;
            NV_ENC_REGISTERED_PTR registered_ = nullptr;
            NV_ENC_OUTPUT_PTR bitstream_ = nullptr;
            uint32_t timingFrames_ = 0;
        };

    } // namespace

    std::unique_ptr<Encoder> CreateNvencEncoder() { return std::make_unique<NvencEncoder>(); }

} // namespace pd
