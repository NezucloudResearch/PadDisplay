// PadDisplay by Nezucloud. Copyright (c) 2026 Nezucloud.
// SPDX-License-Identifier: MIT
#include "capture.h"
#include "log.h"

#include <d3dcompiler.h>
#include <algorithm>

namespace pd {

bool FindOutput(const std::wstring& gdiName, CaptureTarget& target) {
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return false;
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT a = 0; factory->EnumAdapters1(a, &adapter) != DXGI_ERROR_NOT_FOUND; ++a) {
        ComPtr<IDXGIOutput> out;
        for (UINT o = 0; adapter->EnumOutputs(o, &out) != DXGI_ERROR_NOT_FOUND; ++o) {
            DXGI_OUTPUT_DESC od;
            out->GetDesc(&od);
            if (_wcsicmp(od.DeviceName, gdiName.c_str()) != 0) continue;
            DXGI_ADAPTER_DESC1 ad;
            adapter->GetDesc1(&ad);
            target.adapter = adapter;
            out.As(&target.output);
            target.desktopRect = od.DesktopCoordinates;
            target.vendorId = ad.VendorId;
            target.adapterName = ad.Description;
            return target.output != nullptr;
        }
    }
    return false;
}

// D3DKMTSetProcessSchedulingPriorityClass (gdi32): lets our copy/encode work preempt normal GPU work.
// High is enough next to desktop apps, but a game that keeps the GPU at 100% still starves it
// (a frame took 30-40 ms instead of 6). Gaming mode asks for realtime, as Sunshine does. It is
// opt-in: with realtime the NVIDIA encoder is known to freeze when video memory runs out.
static void RaiseGpuPriority(bool realtime) {
    using Fn = LONG(APIENTRY*)(HANDLE, int);
    const int kHigh = 4, kRealtime = 5; // D3DKMT_SCHEDULINGPRIORITYCLASS_HIGH / _REALTIME
    static int applied = 0;
    int want = realtime ? kRealtime : kHigh;
    if (applied == want) return;
    applied = want;
    auto fn = reinterpret_cast<Fn>(GetProcAddress(GetModuleHandleW(L"gdi32.dll"), "D3DKMTSetProcessSchedulingPriorityClass"));
    if (!fn) return;
    LONG st = fn(GetCurrentProcess(), want);
    LOGI("gpu scheduling priority %s -> 0x%08lx", realtime ? "REALTIME (gaming mode)" : "HIGH", st);
    if (st != 0 && realtime) { // not allowed on this system
        applied = kHigh;
        st = fn(GetCurrentProcess(), kHigh);
        LOGI("gpu scheduling priority HIGH -> 0x%08lx", st);
    }
}

bool CreateDevice(IDXGIAdapter1* adapter, bool gamingMode, ComPtr<ID3D11Device>& device, ComPtr<ID3D11DeviceContext>& ctx) {
    RaiseGpuPriority(gamingMode);
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
    HRESULT hr = D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags, levels, 2, D3D11_SDK_VERSION,
                                   &device, nullptr, &ctx);
    if (FAILED(hr)) {
        LOGI("D3D11CreateDevice failed 0x%08lx", hr);
        return false;
    }
    ComPtr<ID3D10Multithread> mt;
    if (SUCCEEDED(ctx.As(&mt))) mt->SetMultithreadProtected(TRUE);
    ComPtr<IDXGIDevice> dxgiDev;
    if (SUCCEEDED(device.As(&dxgiDev))) dxgiDev->SetGPUThreadPriority(7); // best effort, may need privileges
    return true;
}

HRESULT DesktopCapture::Init(ID3D11Device* device, IDXGIOutput1* output, bool drawCursor) {
    device_ = device;
    device_->GetImmediateContext(&ctx_);
    drawCursor_ = drawCursor;
    dupl_.Reset();
    filled_ = nullptr;
    rtvFor_ = nullptr;
    rtv_.Reset();
    underSaved_ = false;

    HRESULT hr = E_FAIL;
    ComPtr<IDXGIOutput5> out5;
    if (SUCCEEDED(output->QueryInterface(IID_PPV_ARGS(&out5)))) {
        const DXGI_FORMAT fmts[] = {DXGI_FORMAT_B8G8R8A8_UNORM};
        hr = out5->DuplicateOutput1(device, 0, 1, fmts, &dupl_);
    }
    if (FAILED(hr)) hr = output->DuplicateOutput(device, &dupl_);
    if (FAILED(hr)) return hr;

    DXGI_OUTDUPL_DESC dd;
    dupl_->GetDesc(&dd);
    width_ = int(dd.ModeDesc.Width);
    height_ = int(dd.ModeDesc.Height);
    if (dd.Rotation != DXGI_MODE_ROTATION_IDENTITY && dd.Rotation != DXGI_MODE_ROTATION_UNSPECIFIED)
        LOGI("capture: output is rotated (%d); rotation is not supported, set the virtual display to landscape", dd.Rotation);

    if (drawCursor_ && !vs_ && !InitCursorPipeline()) drawCursor_ = false;
    LOGI("capture: duplicating %dx%d (format %d)", width_, height_, dd.ModeDesc.Format);
    return S_OK;
}

DesktopCapture::Result DesktopCapture::Next(UINT timeoutMs, ID3D11Texture2D* dst, bool force, bool& fresh) {
    fresh = false;
    DXGI_OUTDUPL_FRAME_INFO info{};
    ComPtr<IDXGIResource> res;
    HRESULT hr = dupl_->AcquireNextFrame(timeoutMs, &info, &res);
    if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
        if (!force || filled_ != dst) return Timeout;
        lastCopied_ = 0;
        DrawCursor(dst); // dst already holds the clean desktop
        return Frame;
    }
    if (hr == DXGI_ERROR_ACCESS_LOST || hr == DXGI_ERROR_INVALID_CALL) return Lost;
    if (FAILED(hr)) {
        LOGI("capture: AcquireNextFrame failed 0x%08lx", hr);
        return Failed;
    }

    bool desktopChanged = info.LastPresentTime.QuadPart != 0;
    bool cursorChanged = false;
    lastCopied_ = 0;
    if (desktopChanged) {
        ComPtr<ID3D11Texture2D> tex;
        res.As(&tex);
        if (filled_ != dst) {
            ctx_->CopyResource(dst, tex.Get()); // first frame after (re)start: everything
            filled_ = dst;
            lastCopied_ = 1;
        } else {
            CopyChanged(tex.Get(), dst, info);
        }
    }
    if (info.LastMouseUpdateTime.QuadPart != 0) {
        cursorChanged = cursorVisible_ != (info.PointerPosition.Visible != FALSE) ||
                        cursorPos_.x != info.PointerPosition.Position.x || cursorPos_.y != info.PointerPosition.Position.y;
        cursorVisible_ = info.PointerPosition.Visible != FALSE;
        cursorPos_ = info.PointerPosition.Position;
    }
    if (info.PointerShapeBufferSize > 0) {
        shapeBuf_.resize(info.PointerShapeBufferSize);
        UINT needed = 0;
        DXGI_OUTDUPL_POINTER_SHAPE_INFO si{};
        if (SUCCEEDED(dupl_->GetFramePointerShape(UINT(shapeBuf_.size()), shapeBuf_.data(), &needed, &si))) {
            UpdateCursorShape(si, shapeBuf_);
            cursorChanged = true;
        }
    }
    dupl_->ReleaseFrame();

    if (filled_ != dst) return Timeout; // no full desktop yet
    fresh = desktopChanged || (drawCursor_ && cursorChanged);
    if (!fresh && !force) return Timeout;
    DrawCursor(dst);
    return Frame;
}

// Copies only what changed since the last frame: destinations of moved regions (scrolling, window
// drags) and dirty rectangles, straight from the new desktop image. Falls back to a full copy when
// the metadata is unavailable or most of the screen changed anyway.
void DesktopCapture::CopyChanged(ID3D11Texture2D* src, ID3D11Texture2D* dst, const DXGI_OUTDUPL_FRAME_INFO& info) {
    auto full = [&] {
        ctx_->CopyResource(dst, src);
        lastCopied_ = 1;
    };
    if (info.TotalMetadataBufferSize == 0) return full();
    meta_.resize(info.TotalMetadataBufferSize);
    rects_.clear();
    UINT moveBytes = 0, dirtyBytes = 0;
    if (FAILED(dupl_->GetFrameMoveRects(UINT(meta_.size()), reinterpret_cast<DXGI_OUTDUPL_MOVE_RECT*>(meta_.data()), &moveBytes)))
        return full();
    auto* moves = reinterpret_cast<const DXGI_OUTDUPL_MOVE_RECT*>(meta_.data());
    for (UINT i = 0; i < moveBytes / sizeof(DXGI_OUTDUPL_MOVE_RECT); ++i) rects_.push_back(moves[i].DestinationRect);
    if (FAILED(dupl_->GetFrameDirtyRects(UINT(meta_.size() - moveBytes), reinterpret_cast<RECT*>(meta_.data() + moveBytes), &dirtyBytes)))
        return full();
    auto* dirty = reinterpret_cast<const RECT*>(meta_.data() + moveBytes);
    rects_.insert(rects_.end(), dirty, dirty + dirtyBytes / sizeof(RECT));

    long long area = 0;
    for (auto& r : rects_) area += 1LL * std::max<LONG>(0, r.right - r.left) * std::max<LONG>(0, r.bottom - r.top);
    const long long screen = 1LL * width_ * height_;
    if (rects_.size() > 256 || area * 10 > screen * 6) return full(); // one big copy beats many small ones
    for (auto& r : rects_) {
        D3D11_BOX b{UINT(std::clamp<LONG>(r.left, 0, width_)), UINT(std::clamp<LONG>(r.top, 0, height_)), 0,
                    UINT(std::clamp<LONG>(r.right, 0, width_)), UINT(std::clamp<LONG>(r.bottom, 0, height_)), 1};
        if (b.right > b.left && b.bottom > b.top) ctx_->CopySubresourceRegion(dst, 0, b.left, b.top, 0, src, 0, &b);
    }
    lastCopied_ = float(double(area) / double(screen));
}

void DesktopCapture::RestoreCursorArea(ID3D11Texture2D* dst) {
    if (!underSaved_) return;
    underSaved_ = false;
    D3D11_BOX b{0, 0, 0, underBox_.right - underBox_.left, underBox_.bottom - underBox_.top, 1};
    ctx_->CopySubresourceRegion(dst, 0, underBox_.left, underBox_.top, 0, under_.Get(), 0, &b);
}

static const char kCursorHlsl[] = R"(
cbuffer CB : register(b0) { float4 rect; };
Texture2D tex : register(t0);
SamplerState smp : register(s0);
struct VO { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VO vs(uint id : SV_VertexID) {
    float2 uv = float2(id & 1, id >> 1);
    VO o;
    o.pos = float4(lerp(rect.x, rect.z, uv.x), lerp(rect.y, rect.w, uv.y), 0, 1);
    o.uv = uv;
    return o;
}
float4 ps(VO i) : SV_Target { return tex.Sample(smp, i.uv); }
)";

bool DesktopCapture::InitCursorPipeline() {
    ComPtr<ID3DBlob> vsb, psb, err;
    if (FAILED(D3DCompile(kCursorHlsl, sizeof(kCursorHlsl) - 1, "cursor", nullptr, nullptr, "vs", "vs_4_0", 0, 0, &vsb, &err)) ||
        FAILED(D3DCompile(kCursorHlsl, sizeof(kCursorHlsl) - 1, "cursor", nullptr, nullptr, "ps", "ps_4_0", 0, 0, &psb, &err))) {
        LOGI("capture: cursor shader compile failed: %s", err ? static_cast<const char*>(err->GetBufferPointer()) : "?");
        return false;
    }
    device_->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &vs_);
    device_->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &ps_);

    D3D11_BUFFER_DESC bd{16, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER};
    device_->CreateBuffer(&bd, nullptr, &cb_);

    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    device_->CreateSamplerState(&sd, &sampler_);

    D3D11_BLEND_DESC blend{};
    auto& rt = blend.RenderTarget[0];
    rt.BlendEnable = TRUE;
    rt.SrcBlend = D3D11_BLEND_SRC_ALPHA;
    rt.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    rt.BlendOp = D3D11_BLEND_OP_ADD;
    rt.SrcBlendAlpha = D3D11_BLEND_ZERO;
    rt.DestBlendAlpha = D3D11_BLEND_ONE;
    rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
    rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    device_->CreateBlendState(&blend, &blendAlpha_);
    // result = src * (1 - dst) + dst * (1 - src): inverts wherever the mask is white.
    rt.SrcBlend = D3D11_BLEND_INV_DEST_COLOR;
    rt.DestBlend = D3D11_BLEND_INV_SRC_COLOR;
    device_->CreateBlendState(&blend, &blendInvert_);
    return vs_ && ps_ && cb_ && sampler_ && blendAlpha_ && blendInvert_;
}

void DesktopCapture::UpdateCursorShape(const DXGI_OUTDUPL_POINTER_SHAPE_INFO& si, const std::vector<uint8_t>& buf) {
    if (!drawCursor_) return;
    int w = int(si.Width);
    int h = si.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME ? int(si.Height / 2) : int(si.Height);
    if (w <= 0 || h <= 0) return;
    std::vector<uint32_t> color(size_t(w) * h, 0), invert(size_t(w) * h, 0);
    bool hasInvert = false;

    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            uint32_t& c = color[size_t(y) * w + x];
            uint32_t& inv = invert[size_t(y) * w + x];
            switch (si.Type) {
            case DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR:
                c = *reinterpret_cast<const uint32_t*>(&buf[size_t(y) * si.Pitch + x * 4]);
                break;
            case DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME: {
                int shift = 7 - (x % 8);
                int andBit = (buf[size_t(y) * si.Pitch + x / 8] >> shift) & 1;
                int xorBit = (buf[size_t(y + h) * si.Pitch + x / 8] >> shift) & 1;
                if (!andBit) c = xorBit ? 0xFFFFFFFFu : 0xFF000000u;
                else if (xorBit) inv = 0xFFFFFFFFu, hasInvert = true;
                break;
            }
            case DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MASKED_COLOR: {
                uint32_t px = *reinterpret_cast<const uint32_t*>(&buf[size_t(y) * si.Pitch + x * 4]);
                if ((px >> 24) == 0) c = px | 0xFF000000u;
                else if (px & 0xFFFFFF) inv = px | 0xFF000000u, hasInvert = true;
                break;
            }
            default:
                break;
            }
        }
    }

    auto makeSrv = [&](const std::vector<uint32_t>& px, ComPtr<ID3D11ShaderResourceView>& srv) {
        D3D11_TEXTURE2D_DESC td{};
        td.Width = w;
        td.Height = h;
        td.MipLevels = td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_IMMUTABLE;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA init{px.data(), UINT(w * 4), 0};
        ComPtr<ID3D11Texture2D> tex;
        srv.Reset();
        if (SUCCEEDED(device_->CreateTexture2D(&td, &init, &tex))) device_->CreateShaderResourceView(tex.Get(), nullptr, &srv);
    };
    makeSrv(color, cursorColor_);
    if (hasInvert) makeSrv(invert, cursorInvert_);
    D3D11_TEXTURE2D_DESC ud{};
    ud.Width = w;
    ud.Height = h;
    ud.MipLevels = ud.ArraySize = 1;
    ud.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    ud.SampleDesc.Count = 1;
    under_.Reset();
    underSaved_ = false; // a new shape arrives before the next DrawCursor
    device_->CreateTexture2D(&ud, nullptr, &under_);
    cursorHasInvert_ = hasInvert;
    cursorW_ = w;
    cursorH_ = h;
}

void DesktopCapture::DrawCursor(ID3D11Texture2D* dst) {
    if (!drawCursor_ || !cursorVisible_ || !cursorColor_ || underSaved_) return;
    D3D11_BOX box{UINT(std::clamp<LONG>(cursorPos_.x, 0, width_)), UINT(std::clamp<LONG>(cursorPos_.y, 0, height_)), 0,
                  UINT(std::clamp<LONG>(cursorPos_.x + cursorW_, 0, width_)), UINT(std::clamp<LONG>(cursorPos_.y + cursorH_, 0, height_)), 1};
    if (box.right <= box.left || box.bottom <= box.top) return; // off this screen
    if (rtvFor_ != dst) {
        rtv_.Reset();
        if (FAILED(device_->CreateRenderTargetView(dst, nullptr, &rtv_))) return;
        rtvFor_ = dst;
    }
    ctx_->CopySubresourceRegion(under_.Get(), 0, 0, 0, 0, dst, 0, &box);
    underBox_ = box;
    underSaved_ = true;
    ID3D11RenderTargetView* rtv = rtv_.Get();

    float rect[4] = {
        float(cursorPos_.x) / width_ * 2.f - 1.f,
        1.f - float(cursorPos_.y) / height_ * 2.f,
        float(cursorPos_.x + cursorW_) / width_ * 2.f - 1.f,
        1.f - float(cursorPos_.y + cursorH_) / height_ * 2.f,
    };
    ctx_->UpdateSubresource(cb_.Get(), 0, nullptr, rect, 0, 0);
    D3D11_VIEWPORT vp{0, 0, float(width_), float(height_), 0, 1};
    ctx_->RSSetViewports(1, &vp);
    ctx_->OMSetRenderTargets(1, &rtv, nullptr);
    ctx_->IASetInputLayout(nullptr);
    ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    ctx_->VSSetShader(vs_.Get(), nullptr, 0);
    ctx_->VSSetConstantBuffers(0, 1, cb_.GetAddressOf());
    ctx_->PSSetShader(ps_.Get(), nullptr, 0);
    ctx_->PSSetSamplers(0, 1, sampler_.GetAddressOf());

    const float factor[4] = {};
    ctx_->OMSetBlendState(blendAlpha_.Get(), factor, 0xFFFFFFFF);
    ctx_->PSSetShaderResources(0, 1, cursorColor_.GetAddressOf());
    ctx_->Draw(4, 0);
    if (cursorHasInvert_ && cursorInvert_) {
        ctx_->OMSetBlendState(blendInvert_.Get(), factor, 0xFFFFFFFF);
        ctx_->PSSetShaderResources(0, 1, cursorInvert_.GetAddressOf());
        ctx_->Draw(4, 0);
    }
    ID3D11ShaderResourceView* nullSrv = nullptr;
    ctx_->PSSetShaderResources(0, 1, &nullSrv);
    ctx_->OMSetRenderTargets(0, nullptr, nullptr);
}

} // namespace pd
