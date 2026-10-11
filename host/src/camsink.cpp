// PadDisplay by Nezucloud. Copyright (c) 2026 Nezucloud.
// SPDX-License-Identifier: MIT
#include "camsink.h"
#include "camshared.h"
#include "log.h"
#include "protocol.h"
#include "vcam.h"

#include <windows.h>
#include <codecapi.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <wmcodecdsp.h>
#include <wrl/client.h>
#include <algorithm>
#include <cstring>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace pd {
namespace {

constexpr uint32_t kMaxCamPayload = 4 * 1024 * 1024; // an IDR at 1080p30 is a few hundred KB

// The Media Foundation H.264 decoder in low-latency mode (it outputs a frame as soon as it can, with no reorder delay:
// the tablet sends no B-frames). It decodes in software: 720p/1080p30 costs a few percent of one core, and a hardware
// decoder would need a Direct3D device manager shared with the encoder's device for no visible gain at this size.
class Decoder {
public:
    bool Init(uint32_t w, uint32_t h, uint32_t fps) {
        if (FAILED(CoCreateInstance(CLSID_CMSH264DecoderMFT, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&mft_)))) return false;
        // MF_LOW_LATENCY is the decoder's own switch for "give me each frame as soon as it is decoded"; without it
        // the decoder holds several frames back before outputting any.
        ComPtr<IMFAttributes> attrs;
        if (SUCCEEDED(mft_->GetAttributes(&attrs))) attrs->SetUINT32(MF_LOW_LATENCY, TRUE);
        ComPtr<ICodecAPI> api;
        if (SUCCEEDED(mft_.As(&api))) {
            VARIANT v;
            VariantInit(&v);
            v.vt = VT_BOOL;
            v.boolVal = VARIANT_TRUE;
            api->SetValue(&CODECAPI_AVLowLatencyMode, &v);
        }
        ComPtr<IMFMediaType> in;
        MFCreateMediaType(&in);
        in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        in->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
        in->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        MFSetAttributeSize(in.Get(), MF_MT_FRAME_SIZE, w, h);
        MFSetAttributeRatio(in.Get(), MF_MT_FRAME_RATE, fps, 1);
        if (FAILED(mft_->SetInputType(0, in.Get(), 0))) return false;
        if (!ChooseOutput()) return false;
        mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
        mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
        return true;
    }

    void Flush() {
        if (mft_) mft_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
    }

    // Feeds one Annex-B access unit (or the SPS/PPS). A new sample per unit: the decoder may keep the buffer.
    bool Feed(const uint8_t* data, size_t n, int64_t ptsUs, std::vector<uint8_t>& scratch) {
        (void)scratch;
        ComPtr<IMFSample> sample;
        ComPtr<IMFMediaBuffer> buf;
        if (FAILED(MFCreateSample(&sample)) || FAILED(MFCreateMemoryBuffer(DWORD(n), &buf))) return false;
        BYTE* p = nullptr;
        if (FAILED(buf->Lock(&p, nullptr, nullptr))) return false;
        memcpy(p, data, n);
        buf->Unlock();
        buf->SetCurrentLength(DWORD(n));
        sample->AddBuffer(buf.Get());
        sample->SetSampleTime(ptsUs * 10);
        HRESULT hr = mft_->ProcessInput(0, sample.Get(), 0);
        return SUCCEEDED(hr) || hr == MF_E_NOTACCEPTING;
    }

    // Calls onFrame(y, strideY, uv, strideUv, w, h) for every decoded frame; returns false on a decoder error.
    template <class F>
    bool Drain(F&& onFrame) {
        for (;;) {
            MFT_OUTPUT_STREAM_INFO info{};
            mft_->GetOutputStreamInfo(0, &info);
            const bool provides = (info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
            MFT_OUTPUT_DATA_BUFFER out{};
            if (!provides) {
                if (!own_ || ownSize_ < info.cbSize) {
                    own_.Reset();
                    ComPtr<IMFMediaBuffer> b;
                    if (FAILED(MFCreateSample(&own_)) || FAILED(MFCreateMemoryBuffer(std::max<DWORD>(info.cbSize, 1), &b))) return false;
                    own_->AddBuffer(b.Get());
                    ownSize_ = info.cbSize;
                }
                own_->GetBufferByIndex(0, &ownBuf_);
                ownBuf_->SetCurrentLength(0);
                out.pSample = own_.Get();
            }
            DWORD status = 0;
            HRESULT hr = mft_->ProcessOutput(0, 1, &out, &status);
            if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) return true;
            if (hr == MF_E_TRANSFORM_STREAM_CHANGE) { // the stream's size changed
                if (!ChooseOutput()) return false;
                continue;
            }
            if (FAILED(hr)) return false;
            IMFSample* s = out.pSample;
            ComPtr<IMFMediaBuffer> buf;
            if (s && SUCCEEDED(s->ConvertToContiguousBuffer(&buf))) {
                BYTE* data = nullptr;
                DWORD len = 0;
                if (SUCCEEDED(buf->Lock(&data, nullptr, &len))) {
                    // The buffer's rows are padded and its height is rounded up (1080 -> 1088); the UV plane starts after
                    // the padded Y plane.
                    const uint32_t alignedH = stride_ ? len * 2 / 3 / stride_ : 0;
                    if (alignedH >= h_) onFrame(data, stride_, data + size_t(stride_) * alignedH, stride_, w_, h_);
                    buf->Unlock();
                }
            }
            if (provides && out.pSample) out.pSample->Release();
            if (out.pEvents) out.pEvents->Release();
        }
    }

private:
    bool ChooseOutput() {
        for (DWORD i = 0;; ++i) {
            ComPtr<IMFMediaType> t;
            if (FAILED(mft_->GetOutputAvailableType(0, i, &t))) return false;
            GUID sub{};
            t->GetGUID(MF_MT_SUBTYPE, &sub);
            if (sub != MFVideoFormat_NV12) continue;
            if (FAILED(mft_->SetOutputType(0, t.Get(), 0))) return false;
            UINT32 fw = 0, fh = 0;
            MFGetAttributeSize(t.Get(), MF_MT_FRAME_SIZE, &fw, &fh);
            w_ = fw;
            h_ = fh;
            MFVideoArea area{};
            if (SUCCEEDED(t->GetBlob(MF_MT_MINIMUM_DISPLAY_APERTURE, reinterpret_cast<UINT8*>(&area), sizeof(area), nullptr)) && area.Area.cx > 0 && area.Area.cy > 0) {
                w_ = UINT32(area.Area.cx);
                h_ = UINT32(area.Area.cy);
            }
            w_ &= ~1u; // NV12 chroma is subsampled by two both ways
            h_ &= ~1u;
            LONG stride = 0;
            if (FAILED(MFGetStrideForBitmapInfoHeader(MFVideoFormat_NV12.Data1, fw, &stride)) || stride <= 0) stride = LONG(fw);
            stride_ = UINT32(stride);
            UINT32 declared = 0;
            if (SUCCEEDED(t->GetUINT32(MF_MT_DEFAULT_STRIDE, &declared)) && declared > 0) stride_ = declared;
            return true;
        }
    }

    ComPtr<IMFTransform> mft_;
    ComPtr<IMFSample> own_;
    ComPtr<IMFMediaBuffer> ownBuf_;
    DWORD ownSize_ = 0;
    uint32_t w_ = 0, h_ = 0, stride_ = 0;
};

} // namespace

CamSession::CamSession(Conn conn, const Settings& settings, uint16_t width, uint16_t height, uint8_t fps, StatusFn status)
    : conn_(std::move(conn)), settings_(settings), width_(width), height_(height), fps_(fps), status_(std::move(status)) {
    stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
}

CamSession::~CamSession() {
    Stop();
    if (view_.load()) UnmapViewOfFile(view_.load());
    if (mapping_) CloseHandle(static_cast<HANDLE>(mapping_));
    if (stopEvent_) CloseHandle(static_cast<HANDLE>(stopEvent_));
    CloseSocket();
}

void CamSession::Stop() {
    stop_ = true;
    if (stopEvent_) SetEvent(static_cast<HANDLE>(stopEvent_));
    std::lock_guard lock(socketMutex_);
    if (conn_.s != INVALID_SOCKET) shutdown(conn_.s, SD_BOTH); // unblocks the receive
}

void CamSession::CloseSocket() {
    std::lock_guard lock(socketMutex_);
    if (conn_.s != INVALID_SOCKET) closesocket(conn_.s);
    conn_.s = INVALID_SOCKET;
}

void CamSession::SendNotice(const char* text) {
    std::lock_guard lock(sendMutex_);
    SendMsg(conn_, Msg::Notice, text, uint32_t(strlen(text)));
    if (status_) status_(L"Camera: " + Widen(text));
}

void CamSession::SendState(bool active) {
    std::lock_guard lock(sendMutex_);
    const uint8_t v = active ? 1 : 0;
    SendMsg(conn_, Msg::CamState, &v, 1);
}

// The Frame Server creates the mailbox when an app opens the camera, and it only exists while someone holds it open, so
// this is retried until it appears. The polling interval (250 ms) is the delay between an app opening the webcam and the
// tablet's camera starting; it only runs while a tablet's camera connection exists, never when idle.
bool CamSession::OpenMailbox() {
    if (view_.load()) return true;
    HANDLE m = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, kCamMapName);
    if (!m) return false;
    auto* v = static_cast<CamShared*>(MapViewOfFile(m, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, kCamMapBytes));
    if (!v || v->magic != kCamMagic) {
        if (v) UnmapViewOfFile(v);
        CloseHandle(m);
        return false;
    }
    mapping_ = m;
    view_ = v;
    return true;
}

void CamSession::Watch() {
    while (WaitForSingleObject(static_cast<HANDLE>(stopEvent_), 250) == WAIT_TIMEOUT) {
        bool active = false;
        if (OpenMailbox()) {
            const uint64_t now = GetTickCount64();
            active = now - static_cast<uint64_t>(view_.load()->readerTick) < kCamReaderStaleMs;
        }
        if (active != active_.load()) {
            active_ = active;
            SendState(active);
            LOGI("camera: a PC app %s the webcam", active ? "opened" : "closed");
        }
    }
}

// Seqlock write: the reader (another process) never waits for us and we never wait for it.
void CamSession::Publish(const uint8_t* y, uint32_t strideY, const uint8_t* uv, uint32_t strideUv, uint32_t w, uint32_t h, int64_t ptsUs) {
    CamShared* s = view_.load();
    if (!s || w > kCamMaxWidth || h > kCamMaxHeight) return;
    s->seq = s->seq + 1; // odd: being written
    MemoryBarrier();
    s->width = int32_t(w);
    s->height = int32_t(h);
    uint8_t* dst = CamPixels(s);
    for (uint32_t r = 0; r < h; ++r) memcpy(dst + size_t(r) * w, y + size_t(r) * strideY, w);
    uint8_t* dstUv = dst + size_t(w) * h;
    for (uint32_t r = 0; r < h / 2; ++r) memcpy(dstUv + size_t(r) * w, uv + size_t(r) * strideUv, w);
    s->ptsUs = ptsUs;
    s->writeTick = int64_t(GetTickCount64());
    MemoryBarrier();
    s->seq = s->seq + 1; // even: stable
    ++framesOut_;
}

void CamSession::Run() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION, MFSTARTUP_LITE);
    MakeRealtimeThread(L"Capture");

    // Every way out ends the connection cleanly, after any Notice has gone out, so the tablet stops its camera.
    auto end = [&] {
        LOGI("camera: ended (%llu frames in, %llu published, %llu decode errors)", framesIn_, framesOut_, decodeErrors_);
        FinishConnection(conn_.s, 1000);
        CloseSocket();
        MFShutdown();
        CoUninitialize();
    };
    if (!settings_.acceptTabletCamera) {
        SendNotice("The camera is switched off in PadDisplay on the PC (settings.ini: acceptTabletCamera).");
        return end();
    }
    const CamState state = QueryCamera();
    if (state == CamState::Unsupported) {
        SendNotice("The tablet's camera as a PC webcam needs Windows 11.");
        return end();
    }
    if (state == CamState::NotInstalled) {
        SendNotice("The virtual camera is not set up on the PC. Right-click the PadDisplay tray icon and choose 'Finish PadDisplay setup'.");
        return end();
    }
    if (width_ == 0 || height_ == 0 || width_ > kCamMaxWidth || height_ > kCamMaxHeight) {
        SendNotice("The camera size is not supported (up to 1920x1080).");
        return end();
    }
    Decoder decoder;
    if (!decoder.Init(width_, height_, fps_ ? fps_ : 30)) {
        SendNotice("This PC could not start an H.264 decoder for the camera.");
        return end();
    }
    LOGI("camera: tablet camera %ux%u@%u", width_, height_, fps_);

    SendState(false); // nothing is captured until a PC app opens the webcam
    std::thread watcher([this] { Watch(); });
    DWORD noTimeout = 0; // the tablet sends nothing while no app has the webcam open; TCP keepalive catches a vanished tablet
    setsockopt(conn_.s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<char*>(&noTimeout), sizeof(noTimeout));

    Msg type{};
    std::vector<uint8_t> payload, scratch;
    bool needKey = true;
    while (!stop_) {
        if (!RecvMsg(conn_, type, payload, kMaxCamPayload)) break;
        if (type == Msg::CamConfig) {
            decoder.Flush();
            needKey = true;
            decoder.Feed(payload.data(), payload.size(), 0, scratch);
        } else if (type == Msg::CamFrame && payload.size() > 9) {
            if (!active_.load()) continue; // nobody is watching: a late frame from before the tablet stopped
            const int64_t pts = int64_t(rd64(payload.data()));
            const bool key = (payload[8] & 1) != 0;
            if (needKey && !key) continue; // nothing to decode against yet
            needKey = false;
            ++framesIn_;
            if (!decoder.Feed(payload.data() + 9, payload.size() - 9, pts, scratch) ||
                !decoder.Drain([&](const uint8_t* y, uint32_t sy, const uint8_t* uv, uint32_t suv, uint32_t w, uint32_t h) { Publish(y, sy, uv, suv, w, h, pts); })) {
                ++decodeErrors_;
                needKey = true;
                decoder.Flush();
                std::lock_guard lock(sendMutex_);
                SendMsg(conn_, Msg::CamKeyframeReq, nullptr, 0);
            }
        }
    }
    stop_ = true;
    SetEvent(static_cast<HANDLE>(stopEvent_));
    if (watcher.joinable()) watcher.join();
    end();
}

} // namespace pd
