// PadDisplay by Nezucloud. Copyright (c) 2026 Nezucloud.
// SPDX-License-Identifier: MIT
#include "cursor_overlay.h"
#include "log.h"

#include <d3dcompiler.h>
#include <algorithm>
#include <cstring>

namespace pd
{
    namespace
    {

        constexpr uint32_t kOpaque = 0xFF000000u;
        constexpr uint32_t kOpaqueWhite = 0xFFFFFFFFu;

        const char kCursorHlsl[] = R"(
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

        uint32_t ReadPixel(const std::vector<uint8_t> &shape, size_t offset)
        {
            uint32_t pixel;
            memcpy(&pixel, &shape[offset], sizeof(pixel));
            return pixel;
        }

        // An AND mask over an XOR mask, one bit per pixel: black or white, transparent, or inverting.
        void DecodeMonochromePixel(const DXGI_OUTDUPL_POINTER_SHAPE_INFO &info, const std::vector<uint8_t> &shape, int x, int y, CursorImage &image, size_t at)
        {
            const int shift = 7 - (x % 8);
            const int andBit = (shape[size_t(y) * info.Pitch + x / 8] >> shift) & 1;
            const int xorBit = (shape[size_t(y + image.height) * info.Pitch + x / 8] >> shift) & 1;
            if (!andBit)
                image.color[at] = xorBit ? kOpaqueWhite : kOpaque;
            else if (xorBit)
            {
                image.invert[at] = kOpaqueWhite;
                image.hasInvert = true;
            }
        }

        // The alpha byte is a mask: 0 replaces the screen pixel, anything else inverts it where the colour is not black.
        void DecodeMaskedPixel(uint32_t pixel, CursorImage &image, size_t at)
        {
            if ((pixel >> 24) == 0)
                image.color[at] = pixel | kOpaque;
            else if (pixel & 0xFFFFFF)
            {
                image.invert[at] = pixel | kOpaque;
                image.hasInvert = true;
            }
        }

        D3D11_BOX ClampedBox(POINT position, int width, int height, int screenW, int screenH)
        {
            return {UINT(std::clamp<LONG>(position.x, 0, screenW)), UINT(std::clamp<LONG>(position.y, 0, screenH)), 0,
                    UINT(std::clamp<LONG>(position.x + width, 0, screenW)), UINT(std::clamp<LONG>(position.y + height, 0, screenH)), 1};
        }

    } // namespace

    CursorImage DecodePointerShape(const DXGI_OUTDUPL_POINTER_SHAPE_INFO &info, const std::vector<uint8_t> &shape)
    {
        CursorImage image;
        const int w = int(info.Width);
        const int h = info.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME ? int(info.Height / 2) : int(info.Height);
        if (w <= 0 || h <= 0)
            return image;
        image.width = w;
        image.height = h;
        image.color.assign(size_t(w) * h, 0);
        image.invert.assign(size_t(w) * h, 0);
        for (int y = 0; y < h; ++y)
        {
            for (int x = 0; x < w; ++x)
            {
                const size_t at = size_t(y) * w + x;
                const size_t rowPixel = size_t(y) * info.Pitch + x * 4;
                switch (info.Type)
                {
                case DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR:
                    image.color[at] = ReadPixel(shape, rowPixel);
                    break;
                case DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME:
                    DecodeMonochromePixel(info, shape, x, y, image, at);
                    break;
                case DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MASKED_COLOR:
                    DecodeMaskedPixel(ReadPixel(shape, rowPixel), image, at);
                    break;
                default:
                    break;
                }
            }
        }
        return image;
    }

    bool CursorOverlay::Init(ID3D11Device *device, ID3D11DeviceContext *ctx)
    {
        device_ = device;
        ctx_ = ctx;
        ComPtr<ID3DBlob> vsb, psb, err;
        if (FAILED(D3DCompile(kCursorHlsl, sizeof(kCursorHlsl) - 1, "cursor", nullptr, nullptr, "vs", "vs_4_0", 0, 0, &vsb, &err)) ||
            FAILED(D3DCompile(kCursorHlsl, sizeof(kCursorHlsl) - 1, "cursor", nullptr, nullptr, "ps", "ps_4_0", 0, 0, &psb, &err)))
        {
            LOGI("capture: cursor shader compile failed: %s", err ? static_cast<const char *>(err->GetBufferPointer()) : "?");
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
        auto &rt = blend.RenderTarget[0];
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

    void CursorOverlay::SetScreenSize(int width, int height)
    {
        screenW_ = width;
        screenH_ = height;
    }

    void CursorOverlay::Reset()
    {
        rtvFor_ = nullptr;
        rtv_.Reset();
        underSaved_ = false;
    }

    ComPtr<ID3D11ShaderResourceView> CursorOverlay::UploadLayer(const std::vector<uint32_t> &pixels, int width, int height)
    {
        D3D11_TEXTURE2D_DESC td{};
        td.Width = UINT(width);
        td.Height = UINT(height);
        td.MipLevels = td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_IMMUTABLE;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA init{pixels.data(), UINT(width * 4), 0};
        ComPtr<ID3D11Texture2D> tex;
        ComPtr<ID3D11ShaderResourceView> view;
        if (SUCCEEDED(device_->CreateTexture2D(&td, &init, &tex)))
            device_->CreateShaderResourceView(tex.Get(), nullptr, &view);
        return view;
    }

    void CursorOverlay::SetShape(const DXGI_OUTDUPL_POINTER_SHAPE_INFO &info, const std::vector<uint8_t> &shape)
    {
        const CursorImage image = DecodePointerShape(info, shape);
        if (image.width == 0)
            return;
        color_ = UploadLayer(image.color, image.width, image.height);
        if (image.hasInvert)
            invert_ = UploadLayer(image.invert, image.width, image.height);
        D3D11_TEXTURE2D_DESC ud{};
        ud.Width = UINT(image.width);
        ud.Height = UINT(image.height);
        ud.MipLevels = ud.ArraySize = 1;
        ud.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        ud.SampleDesc.Count = 1;
        under_.Reset();
        underSaved_ = false; // a new shape arrives before the next Draw
        device_->CreateTexture2D(&ud, nullptr, &under_);
        hasInvert_ = image.hasInvert;
        width_ = image.width;
        height_ = image.height;
    }

    bool CursorOverlay::MoveTo(bool visible, POINT position)
    {
        const bool changed = visible_ != visible || position_.x != position.x || position_.y != position.y;
        visible_ = visible;
        position_ = position;
        return changed;
    }

    void CursorOverlay::Restore(ID3D11Texture2D *dst)
    {
        if (!underSaved_)
            return;
        underSaved_ = false;
        D3D11_BOX b{0, 0, 0, underBox_.right - underBox_.left, underBox_.bottom - underBox_.top, 1};
        ctx_->CopySubresourceRegion(dst, 0, underBox_.left, underBox_.top, 0, under_.Get(), 0, &b);
    }

    bool CursorOverlay::BindTarget(ID3D11Texture2D *dst)
    {
        if (rtvFor_ == dst)
            return true;
        rtv_.Reset();
        if (FAILED(device_->CreateRenderTargetView(dst, nullptr, &rtv_)))
            return false;
        rtvFor_ = dst;
        return true;
    }

    void CursorOverlay::Draw(ID3D11Texture2D *dst)
    {
        if (!visible_ || !color_ || underSaved_)
            return;
        const D3D11_BOX box = ClampedBox(position_, width_, height_, screenW_, screenH_);
        if (box.right <= box.left || box.bottom <= box.top)
            return; // off this screen
        if (!BindTarget(dst))
            return;
        ctx_->CopySubresourceRegion(under_.Get(), 0, 0, 0, 0, dst, 0, &box);
        underBox_ = box;
        underSaved_ = true;
        ID3D11RenderTargetView *rtv = rtv_.Get();

        float rect[4] = {
            float(position_.x) / screenW_ * 2.f - 1.f,
            1.f - float(position_.y) / screenH_ * 2.f,
            float(position_.x + width_) / screenW_ * 2.f - 1.f,
            1.f - float(position_.y + height_) / screenH_ * 2.f,
        };
        ctx_->UpdateSubresource(cb_.Get(), 0, nullptr, rect, 0, 0);
        D3D11_VIEWPORT vp{0, 0, float(screenW_), float(screenH_), 0, 1};
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
        ctx_->PSSetShaderResources(0, 1, color_.GetAddressOf());
        ctx_->Draw(4, 0);
        if (hasInvert_ && invert_)
        {
            ctx_->OMSetBlendState(blendInvert_.Get(), factor, 0xFFFFFFFF);
            ctx_->PSSetShaderResources(0, 1, invert_.GetAddressOf());
            ctx_->Draw(4, 0);
        }
        ID3D11ShaderResourceView *nullSrv = nullptr;
        ctx_->PSSetShaderResources(0, 1, &nullSrv);
        ctx_->OMSetRenderTargets(0, nullptr, nullptr);
    }

} // namespace pd
