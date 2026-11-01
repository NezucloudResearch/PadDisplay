// PadDisplay by Nezucloud. Copyright (c) 2026 Nezucloud.
// SPDX-License-Identifier: MIT
// Draws the mouse pointer into a captured frame. Desktop Duplication hands the pointer over separately from the picture.
#pragma once
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <cstdint>
#include <vector>

namespace pd
{

    using Microsoft::WRL::ComPtr;

    // A pointer in the two layers it is drawn with: blended on top, and inverting what is under it.
    struct CursorImage
    {
        int width = 0, height = 0;
        std::vector<uint32_t> color, invert; // B8G8R8A8, tightly packed
        bool hasInvert = false;
    };

    // Desktop Duplication's three pointer formats. No image (width 0) for a shape with no pixels.
    CursorImage DecodePointerShape(const DXGI_OUTDUPL_POINTER_SHAPE_INFO &info, const std::vector<uint8_t> &shape);

    class CursorOverlay
    {
    public:
        bool Init(ID3D11Device *device, ID3D11DeviceContext *ctx); // false = the pipeline could not be built
        bool Ready() const { return vs_ != nullptr; }
        void SetScreenSize(int width, int height);
        void SetShape(const DXGI_OUTDUPL_POINTER_SHAPE_INFO &info, const std::vector<uint8_t> &shape);
        bool MoveTo(bool visible, POINT position); // true = that is a change
        // Forgets the target it drew into and any pixels it saved: the frames are starting over.
        void Reset();

        // Draws the pointer into dst, saving the pixels under it. Restore puts them back, so dst holds the clean desktop again.
        void Draw(ID3D11Texture2D *dst);
        void Restore(ID3D11Texture2D *dst);

    private:
        bool BindTarget(ID3D11Texture2D *dst);
        ComPtr<ID3D11ShaderResourceView> UploadLayer(const std::vector<uint32_t> &pixels, int width, int height);

        ComPtr<ID3D11Device> device_;
        ComPtr<ID3D11DeviceContext> ctx_;
        int screenW_ = 0, screenH_ = 0;
        bool visible_ = false;
        POINT position_{};
        int width_ = 0, height_ = 0;
        bool hasInvert_ = false;
        ComPtr<ID3D11ShaderResourceView> color_, invert_;
        ComPtr<ID3D11VertexShader> vs_;
        ComPtr<ID3D11PixelShader> ps_;
        ComPtr<ID3D11Buffer> cb_;
        ComPtr<ID3D11SamplerState> sampler_;
        ComPtr<ID3D11BlendState> blendAlpha_, blendInvert_;
        ID3D11Texture2D *rtvFor_ = nullptr;
        ComPtr<ID3D11RenderTargetView> rtv_;
        ComPtr<ID3D11Texture2D> under_; // pixels under the drawn pointer, restored after encoding
        D3D11_BOX underBox_{};
        bool underSaved_ = false;
    };

} // namespace pd
