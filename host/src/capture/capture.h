// PadDisplay by Nezucloud. Copyright (c) 2026 Nezucloud.
// SPDX-License-Identifier: MIT
// DXGI Desktop Duplication of one output, composed with the mouse cursor into a caller texture.
// Or, where Desktop Duplication cannot see the monitor from this process, Windows.Graphics.Capture (capture_wgc.h).
#pragma once
#include "capture_wgc.h"
#include "cursor_overlay.h"

#include <d3d11.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace pd
{

    class DesktopCapture
    {
    public:
        enum Result
        {
            Frame,
            Timeout,
            Lost,
            Failed
        };

        HRESULT Init(ID3D11Device *device, IDXGIOutput1 *output, bool drawCursor);
        // The same through Windows.Graphics.Capture. It copies whole frames, with the pointer drawn by Windows, and does not depend on the
        // GPU this process prefers; the device should be on the GPU that renders the monitor. minIntervalMs: see WgcSource::Create.
        HRESULT InitWgc(ID3D11Device *device, const std::wstring &gdiName, bool drawCursor, unsigned minIntervalMs);
        bool UsesWgc() const { return wgc_ != nullptr; }
        // Waits up to timeoutMs for a desktop or pointer change and brings dst up to date: dst is the
        // encoder's persistent input (same size, B8G8R8A8, render-target bindable). Only the changed
        // rectangles are copied into it, then the cursor is drawn on top. force = compose even if
        // nothing changed; fresh is set when the desktop or cursor actually changed.
        // After encoding dst, call RestoreCursorArea(dst) so it holds the clean desktop again.
        Result Next(UINT timeoutMs, ID3D11Texture2D *dst, bool force, bool &fresh);
        void RestoreCursorArea(ID3D11Texture2D *dst);
        // Share of the screen copied for the last frame (0..1), for diagnostics.
        float LastCopiedFraction() const { return lastCopied_; }
        int Width() const { return width_; }
        int Height() const { return height_; }

    private:
        void StartOver(ID3D11Device *device);
        Result NextWgc(UINT timeoutMs, ID3D11Texture2D *dst, bool force, bool &fresh);
        void CopyDesktop(IDXGIResource *desktop, ID3D11Texture2D *dst, const DXGI_OUTDUPL_FRAME_INFO &info);
        void CopyChanged(ID3D11Texture2D *src, ID3D11Texture2D *dst, const DXGI_OUTDUPL_FRAME_INFO &info);
        bool CollectChangedRects(const DXGI_OUTDUPL_FRAME_INFO &info);
        bool UpdatePointer(const DXGI_OUTDUPL_FRAME_INFO &info); // true = it moved or changed shape

        ComPtr<ID3D11Device> device_;
        ComPtr<ID3D11DeviceContext> ctx_;
        ComPtr<IDXGIOutputDuplication> dupl_;
        std::unique_ptr<WgcSource> wgc_;
        ID3D11Texture2D *filled_ = nullptr; // the dst that currently holds the full desktop
        std::vector<uint8_t> meta_;         // move/dirty rectangle buffer
        std::vector<RECT> rects_;
        float lastCopied_ = 0;
        int width_ = 0, height_ = 0;

        bool drawCursor_ = true;
        CursorOverlay cursor_;
        std::vector<uint8_t> shapeBuf_;
    };

} // namespace pd
