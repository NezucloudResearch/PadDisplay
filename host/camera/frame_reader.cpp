// PadDisplay by Nezucloud. Copyright (c) 2026 Nezucloud.
// SPDX-License-Identifier: MIT
#include "frame_reader.h"

#include <sddl.h>
#include <algorithm>
#include <atomic>
#include <cstring>

namespace pd
{

    // Signed: the host can finish a frame between the reader's clock read and this check, and that frame is the freshest there is.
    bool FrameIsFresh(const CamShared &shared, uint64_t nowMs)
    {
        return static_cast<int64_t>(nowMs) - shared.writeTick <= static_cast<int64_t>(kCamFrameStaleMs);
    }

    // Seqlock read: copy, then check the writer did not touch the slot meanwhile. A torn copy is thrown away and
    // the previous good frame stays.
    void FrameCopy::TakeNewest(CamShared *shared)
    {
        const int64_t seq = shared->seq;
        if (seq & 1)
            return;
        const UINT32 w = static_cast<UINT32>(shared->width), h = static_cast<UINT32>(shared->height);
        if (w < 2 || h < 2 || w > kCamMaxWidth || h > kCamMaxHeight || (w | h) % 2 != 0)
            return;
        std::atomic_thread_fence(std::memory_order_acquire);
        if (back_.size() < kCamCapacity)
            back_.resize(kCamCapacity);
        memcpy(back_.data(), CamPixels(shared), size_t(w) * h * 3 / 2);
        std::atomic_thread_fence(std::memory_order_acquire);
        if (shared->seq != seq)
            return;
        back_.swap(front_);
        width_ = w;
        height_ = h;
    }

    void FrameCopy::Blit(BYTE *dst, LONG pitch, UINT32 w, UINT32 h) const
    {
        const BYTE *srcY = front_.data();
        const BYTE *srcUV = srcY + size_t(width_) * height_;
        if (width_ == w && height_ == h)
        {
            for (UINT32 y = 0; y < h; ++y)
                memcpy(dst + size_t(y) * pitch, srcY + size_t(y) * w, w);
            for (UINT32 y = 0; y < h / 2; ++y)
                memcpy(dst + size_t(h + y) * pitch, srcUV + size_t(y) * w, w);
            return;
        }
        for (UINT32 y = 0; y < h; ++y)
        {
            const BYTE *s = srcY + size_t(uint64_t(y) * height_ / h) * width_;
            BYTE *d = dst + size_t(y) * pitch;
            for (UINT32 x = 0; x < w; ++x)
                d[x] = s[uint64_t(x) * width_ / w];
        }
        for (UINT32 y = 0; y < h / 2; ++y)
        {
            const BYTE *s = srcUV + size_t(uint64_t(y) * (height_ / 2) / (h / 2)) * width_;
            BYTE *d = dst + size_t(h + y) * pitch;
            for (UINT32 x = 0; x < w; x += 2)
            { // U and V stay together
                const UINT32 sx = static_cast<UINT32>(uint64_t(x / 2) * (width_ / 2) / (w / 2)) * 2;
                d[x] = s[sx];
                d[x + 1] = s[sx + 1];
            }
        }
    }

    void DrawNoSignal(BYTE *dst, LONG pitch, UINT32 w, UINT32 h, uint64_t nowMs)
    {
        for (UINT32 y = 0; y < h; ++y)
            memset(dst + size_t(y) * pitch, 0x28, w);
        for (UINT32 y = 0; y < h / 2; ++y)
            memset(dst + size_t(h + y) * pitch, 0x80, w);
        const UINT32 barW = std::max<UINT32>(16, w / 24);
        const UINT32 x0 = static_cast<UINT32>((nowMs / 8) % (w - barW));
        for (UINT32 y = h / 3; y < h / 3 + h / 12; ++y)
            memset(dst + size_t(y) * pitch + x0, 0x90, barW);
    }

    Mailbox::~Mailbox()
    {
        if (view_)
            UnmapViewOfFile(view_);
        if (map_)
            CloseHandle(map_);
    }

    bool Mailbox::Read(BYTE *dst, LONG pitch, UINT32 w, UINT32 h)
    {
        if (!Open())
            return false;
        const uint64_t now = GetTickCount64();
        view_->readerTick = static_cast<int64_t>(now); // tells the host that someone is watching
        if (!FrameIsFresh(*view_, now))
            return false;
        frame_.TakeNewest(view_);
        if (!frame_.HasFrame())
            return false;
        frame_.Blit(dst, pitch, w, h);
        return true;
    }

    // Creates the mapping (this process is a service, so it may use the Global\ namespace) or finds the host's.
    bool Mailbox::Open()
    {
        if (view_)
            return view_->magic == kCamMagic;
        SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, FALSE};
        PSECURITY_DESCRIPTOR sd = nullptr;
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(kCamMapSddl, SDDL_REVISION_1, &sd, nullptr))
            return false;
        sa.lpSecurityDescriptor = sd;
        map_ = CreateFileMappingW(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE, 0, kCamMapBytes, kCamMapName);
        const bool fresh = map_ && GetLastError() != ERROR_ALREADY_EXISTS;
        LocalFree(sd);
        if (!map_)
            return false;
        view_ = static_cast<CamShared *>(MapViewOfFile(map_, FILE_MAP_ALL_ACCESS, 0, 0, kCamMapBytes));
        if (!view_)
        {
            CloseHandle(map_);
            map_ = nullptr;
            return false;
        }
        if (fresh)
        { // pages are zero already; only the identity needs writing
            view_->version = 1;
            view_->capacity = kCamCapacity;
            view_->magic = kCamMagic;
        }
        return view_->magic == kCamMagic;
    }

} // namespace pd
