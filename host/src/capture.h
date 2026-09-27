// PadDisplay by Nezucloud. Copyright (c) 2026 Nezucloud.
// SPDX-License-Identifier: MIT
// DXGI Desktop Duplication of one output, composed with the mouse cursor into a caller texture.
#pragma once
#include <d3d11.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <cstdint>
#include <string>
#include <vector>

namespace pd {

using Microsoft::WRL::ComPtr;

struct CaptureTarget {
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<IDXGIOutput1> output;
    RECT desktopRect{};   // physical pixels in virtual-desktop space
    UINT vendorId = 0;
    std::wstring adapterName;
};

// Finds the DXGI output (and the adapter that renders it) for a GDI source name.
bool FindOutput(const std::wstring& gdiName, CaptureTarget& target);

// Creates a D3D11 device on the given adapter with video support (needed by both encoders).
bool CreateDevice(IDXGIAdapter1* adapter, ComPtr<ID3D11Device>& device, ComPtr<ID3D11DeviceContext>& ctx);

class DesktopCapture {
public:
    enum Result { Frame, Timeout, Lost, Failed };

    HRESULT Init(ID3D11Device* device, IDXGIOutput1* output, bool drawCursor);
    // Waits up to timeoutMs for a desktop or pointer change and brings dst up to date: dst is the
    // encoder's persistent input (same size, B8G8R8A8, render-target bindable). Only the changed
    // rectangles are copied into it, then the cursor is drawn on top. force = compose even if
    // nothing changed; fresh is set when the desktop or cursor actually changed.
    // After encoding dst, call RestoreCursorArea(dst) so it holds the clean desktop again.
    Result Next(UINT timeoutMs, ID3D11Texture2D* dst, bool force, bool& fresh);
    void RestoreCursorArea(ID3D11Texture2D* dst);
    // Share of the screen copied for the last frame (0..1), for diagnostics.
    float LastCopiedFraction() const { return lastCopied_; }
    int Width() const { return width_; }
    int Height() const { return height_; }

private:
    bool InitCursorPipeline();
    void UpdateCursorShape(const DXGI_OUTDUPL_POINTER_SHAPE_INFO& info, const std::vector<uint8_t>& shape);
    void DrawCursor(ID3D11Texture2D* dst);
    void CopyChanged(ID3D11Texture2D* src, ID3D11Texture2D* dst, const DXGI_OUTDUPL_FRAME_INFO& info);

    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> ctx_;
    ComPtr<IDXGIOutputDuplication> dupl_;
    ID3D11Texture2D* filled_ = nullptr; // the dst that currently holds the full desktop
    std::vector<uint8_t> meta_;          // move/dirty rectangle buffer
    std::vector<RECT> rects_;
    float lastCopied_ = 0;
    int width_ = 0, height_ = 0;

    bool drawCursor_ = true;
    bool cursorVisible_ = false;
    POINT cursorPos_{};
    int cursorW_ = 0, cursorH_ = 0;
    bool cursorHasInvert_ = false;
    std::vector<uint8_t> shapeBuf_;
    ComPtr<ID3D11ShaderResourceView> cursorColor_, cursorInvert_;
    ComPtr<ID3D11VertexShader> vs_;
    ComPtr<ID3D11PixelShader> ps_;
    ComPtr<ID3D11Buffer> cb_;
    ComPtr<ID3D11SamplerState> sampler_;
    ComPtr<ID3D11BlendState> blendAlpha_, blendInvert_;
    ID3D11Texture2D* rtvFor_ = nullptr;
    ComPtr<ID3D11RenderTargetView> rtv_;
    ComPtr<ID3D11Texture2D> under_; // pixels under the drawn cursor, restored after encoding
    D3D11_BOX underBox_{};
    bool underSaved_ = false;
};

} // namespace pd
