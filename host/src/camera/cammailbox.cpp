// PadDisplay by Nezucloud. Copyright (c) 2026 Nezucloud.
// SPDX-License-Identifier: MIT
#include "cammailbox.h"
#include "camshared.h"

#include <cstring>

namespace pd
{
    namespace
    {

        void CopyRows(uint8_t *to, const uint8_t *from, uint32_t stride, uint32_t width, uint32_t rows)
        {
            for (uint32_t r = 0; r < rows; ++r)
                memcpy(to + size_t(r) * width, from + size_t(r) * stride, width);
        }

    } // namespace

    bool ReaderIsRecent(int64_t lastAskedMs, uint64_t nowMs)
    {
        // Signed: the reader may stamp its tick just after nowMs was read, and a reader that recent is certainly there.
        return int64_t(nowMs) - lastAskedMs < int64_t(kCamReaderStaleMs);
    }

    // Seqlock write: the reader (another process) never waits for us and we never wait for it.
    bool WriteFrame(CamShared &slot, const Nv12Frame &frame)
    {
        if (frame.width > kCamMaxWidth || frame.height > kCamMaxHeight)
            return false;
        const int64_t writing = slot.seq | 1; // odd: being written. A host that died in mid-write left it odd already.
        slot.seq = writing;
        MemoryBarrier();
        slot.width = int32_t(frame.width);
        slot.height = int32_t(frame.height);
        uint8_t *pixels = CamPixels(&slot);
        CopyRows(pixels, frame.y, frame.stride, frame.width, frame.height);
        CopyRows(pixels + size_t(frame.width) * frame.height, frame.uv, frame.stride, frame.width, frame.height / 2);
        slot.ptsUs = frame.ptsUs;
        slot.writeTick = int64_t(GetTickCount64());
        MemoryBarrier();
        slot.seq = writing + 1; // even: stable
        return true;
    }

    CamMailbox::~CamMailbox()
    {
        if (CamShared *slot = slot_.load())
            UnmapViewOfFile(slot);
        if (mapping_)
            CloseHandle(mapping_);
    }

    // The Frame Server creates the mailbox when an app opens the camera, and it only exists while someone holds it open, so
    // the caller retries until it appears.
    bool CamMailbox::Open()
    {
        if (slot_.load())
            return true;
        HANDLE mapping = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, kCamMapName);
        if (!mapping)
            return false;
        auto *slot = static_cast<CamShared *>(MapViewOfFile(mapping, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, kCamMapBytes));
        if (!slot || slot->magic != kCamMagic)
        {
            if (slot)
                UnmapViewOfFile(slot);
            CloseHandle(mapping);
            return false;
        }
        mapping_ = mapping;
        slot_ = slot;
        return true;
    }

    bool CamMailbox::ReaderIsActive() const
    {
        return ReaderIsRecent(slot_.load()->readerTick, GetTickCount64());
    }

    bool CamMailbox::Publish(const Nv12Frame &frame)
    {
        CamShared *slot = slot_.load();
        return slot && WriteFrame(*slot, frame);
    }

} // namespace pd
