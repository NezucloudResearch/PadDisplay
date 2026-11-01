// PadDisplay by Nezucloud. Copyright (c) 2026 Nezucloud.
// SPDX-License-Identifier: MIT
#include "capture.h"
#include "log.h"

#include <algorithm>

namespace pd
{
    namespace
    {

        // Past either of these, one big copy beats many small ones.
        constexpr size_t kMaxChangedRects = 256;
        constexpr long long kFullCopyTenths = 6; // of the screen

    } // namespace

    void DesktopCapture::StartOver(ID3D11Device *device)
    {
        device_ = device;
        device_->GetImmediateContext(&ctx_);
        dupl_.Reset();
        filled_ = nullptr;
        cursor_.Reset();
    }

    HRESULT DesktopCapture::Init(ID3D11Device *device, IDXGIOutput1 *output, bool drawCursor)
    {
        StartOver(device);
        drawCursor_ = drawCursor;
        wgc_.reset();

        HRESULT hr = E_FAIL;
        ComPtr<IDXGIOutput5> out5;
        if (SUCCEEDED(output->QueryInterface(IID_PPV_ARGS(&out5))))
        {
            const DXGI_FORMAT fmts[] = {DXGI_FORMAT_B8G8R8A8_UNORM};
            hr = out5->DuplicateOutput1(device, 0, 1, fmts, &dupl_);
        }
        if (FAILED(hr))
            hr = output->DuplicateOutput(device, &dupl_);
        if (FAILED(hr))
            return hr;

        DXGI_OUTDUPL_DESC dd;
        dupl_->GetDesc(&dd);
        width_ = int(dd.ModeDesc.Width);
        height_ = int(dd.ModeDesc.Height);
        if (dd.Rotation != DXGI_MODE_ROTATION_IDENTITY && dd.Rotation != DXGI_MODE_ROTATION_UNSPECIFIED)
            LOGI("capture: output is rotated (%d); rotation is not supported, set the virtual display to landscape", dd.Rotation);

        if (drawCursor_ && !cursor_.Ready() && !cursor_.Init(device_.Get(), ctx_.Get()))
            drawCursor_ = false;
        cursor_.SetScreenSize(width_, height_);
        LOGI("capture: duplicating %dx%d (format %d)", width_, height_, dd.ModeDesc.Format);
        return S_OK;
    }

    HRESULT DesktopCapture::InitWgc(ID3D11Device *device, const std::wstring &gdiName, bool drawCursor, unsigned minIntervalMs)
    {
        StartOver(device);
        drawCursor_ = false; // Windows draws the pointer into the frames: nothing of ours to draw or restore
        wgc_ = WgcSource::Create(device, gdiName, drawCursor, minIntervalMs);
        if (!wgc_)
            return E_FAIL;
        width_ = wgc_->Width();
        height_ = wgc_->Height();
        return S_OK;
    }

    DesktopCapture::Result DesktopCapture::NextWgc(UINT timeoutMs, ID3D11Texture2D *dst, bool force, bool &fresh)
    {
        switch (wgc_->Next(timeoutMs, ctx_.Get(), dst))
        {
        case WgcSource::Result::Frame:
            filled_ = dst;
            fresh = true;
            lastCopied_ = 1;
            return Frame;
        case WgcSource::Result::Lost:
            return Lost;
        case WgcSource::Result::Timeout:
            break;
        }
        if (!force || filled_ != dst)
            return Timeout;
        lastCopied_ = 0;
        return Frame; // nothing new, but the caller wants the picture encoded again: dst still holds it
    }

    DesktopCapture::Result DesktopCapture::Next(UINT timeoutMs, ID3D11Texture2D *dst, bool force, bool &fresh)
    {
        fresh = false;
        if (wgc_)
            return NextWgc(timeoutMs, dst, force, fresh);
        DXGI_OUTDUPL_FRAME_INFO info{};
        ComPtr<IDXGIResource> res;
        HRESULT hr = dupl_->AcquireNextFrame(timeoutMs, &info, &res);
        if (hr == DXGI_ERROR_WAIT_TIMEOUT)
        {
            if (!force || filled_ != dst)
                return Timeout;
            lastCopied_ = 0;
            if (drawCursor_)
                cursor_.Draw(dst); // dst already holds the clean desktop
            return Frame;
        }
        // A GPU reset or driver update removes the device: like a lost duplication, the session builds everything again.
        if (hr == DXGI_ERROR_ACCESS_LOST || hr == DXGI_ERROR_INVALID_CALL || hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET)
            return Lost;
        if (FAILED(hr))
        {
            LOGI("capture: AcquireNextFrame failed 0x%08lx", hr);
            return Failed;
        }

        const bool desktopChanged = info.LastPresentTime.QuadPart != 0;
        lastCopied_ = 0;
        if (desktopChanged)
            CopyDesktop(res.Get(), dst, info);
        const bool cursorChanged = UpdatePointer(info);
        dupl_->ReleaseFrame();

        if (filled_ != dst)
            return Timeout; // no full desktop yet
        fresh = desktopChanged || (drawCursor_ && cursorChanged);
        if (!fresh && !force)
            return Timeout;
        if (drawCursor_)
            cursor_.Draw(dst);
        return Frame;
    }

    void DesktopCapture::CopyDesktop(IDXGIResource *desktop, ID3D11Texture2D *dst, const DXGI_OUTDUPL_FRAME_INFO &info)
    {
        ComPtr<ID3D11Texture2D> tex;
        desktop->QueryInterface(IID_PPV_ARGS(&tex));
        if (filled_ == dst)
            return CopyChanged(tex.Get(), dst, info);
        ctx_->CopyResource(dst, tex.Get()); // first frame after (re)start: everything
        filled_ = dst;
        lastCopied_ = 1;
    }

    bool DesktopCapture::UpdatePointer(const DXGI_OUTDUPL_FRAME_INFO &info)
    {
        bool changed = false;
        if (info.LastMouseUpdateTime.QuadPart != 0)
            changed = cursor_.MoveTo(info.PointerPosition.Visible != FALSE, info.PointerPosition.Position);
        if (info.PointerShapeBufferSize > 0)
        {
            shapeBuf_.resize(info.PointerShapeBufferSize);
            UINT needed = 0;
            DXGI_OUTDUPL_POINTER_SHAPE_INFO si{};
            if (SUCCEEDED(dupl_->GetFramePointerShape(UINT(shapeBuf_.size()), shapeBuf_.data(), &needed, &si)))
            {
                if (drawCursor_)
                    cursor_.SetShape(si, shapeBuf_);
                changed = true;
            }
        }
        return changed;
    }

    // Destinations of moved regions (scrolling, window drags) and dirty rectangles. false = the metadata is not to be had.
    bool DesktopCapture::CollectChangedRects(const DXGI_OUTDUPL_FRAME_INFO &info)
    {
        if (info.TotalMetadataBufferSize == 0)
            return false;
        meta_.resize(info.TotalMetadataBufferSize);
        rects_.clear();
        UINT moveBytes = 0, dirtyBytes = 0;
        if (FAILED(dupl_->GetFrameMoveRects(UINT(meta_.size()), reinterpret_cast<DXGI_OUTDUPL_MOVE_RECT *>(meta_.data()), &moveBytes)))
            return false;
        auto *moves = reinterpret_cast<const DXGI_OUTDUPL_MOVE_RECT *>(meta_.data());
        for (UINT i = 0; i < moveBytes / sizeof(DXGI_OUTDUPL_MOVE_RECT); ++i)
            rects_.push_back(moves[i].DestinationRect);
        if (FAILED(dupl_->GetFrameDirtyRects(UINT(meta_.size() - moveBytes), reinterpret_cast<RECT *>(meta_.data() + moveBytes), &dirtyBytes)))
            return false;
        auto *dirty = reinterpret_cast<const RECT *>(meta_.data() + moveBytes);
        rects_.insert(rects_.end(), dirty, dirty + dirtyBytes / sizeof(RECT));
        return true;
    }

    // Copies only what changed since the last frame, straight from the new desktop image. Falls back to a full copy when the
    // metadata is unavailable or most of the screen changed anyway.
    void DesktopCapture::CopyChanged(ID3D11Texture2D *src, ID3D11Texture2D *dst, const DXGI_OUTDUPL_FRAME_INFO &info)
    {
        auto full = [&]
        {
            ctx_->CopyResource(dst, src);
            lastCopied_ = 1;
        };
        if (!CollectChangedRects(info))
            return full();

        long long area = 0;
        for (auto &r : rects_)
            area += 1LL * std::max<LONG>(0, r.right - r.left) * std::max<LONG>(0, r.bottom - r.top);
        const long long screen = 1LL * width_ * height_;
        if (rects_.size() > kMaxChangedRects || area * 10 > screen * kFullCopyTenths)
            return full();
        for (auto &r : rects_)
        {
            D3D11_BOX b{UINT(std::clamp<LONG>(r.left, 0, width_)), UINT(std::clamp<LONG>(r.top, 0, height_)), 0,
                        UINT(std::clamp<LONG>(r.right, 0, width_)), UINT(std::clamp<LONG>(r.bottom, 0, height_)), 1};
            if (b.right > b.left && b.bottom > b.top)
                ctx_->CopySubresourceRegion(dst, 0, b.left, b.top, 0, src, 0, &b);
        }
        lastCopied_ = float(double(area) / double(screen));
    }

    void DesktopCapture::RestoreCursorArea(ID3D11Texture2D *dst) { cursor_.Restore(dst); }

} // namespace pd
