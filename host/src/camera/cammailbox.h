// PadDisplay by Nezucloud. Copyright (c) 2026 Nezucloud.
// SPDX-License-Identifier: MIT
// The host's end of the mailbox to the virtual camera (camshared.h): tells whether a PC app is reading the camera, and
// writes the newest decoded frame.
#pragma once
#include <windows.h>
#include <atomic>
#include <cstdint>

namespace pd
{

    struct CamShared;

    // One decoded picture, valid only during the call that hands it over.
    struct Nv12Frame
    {
        const uint8_t *y;
        const uint8_t *uv;
        uint32_t stride; // bytes per row, the same in both planes
        uint32_t width, height;
        int64_t ptsUs;
    };

    // Did the media source ask for a frame recently enough to still be there?
    bool ReaderIsRecent(int64_t lastAskedMs, uint64_t nowMs);
    // Packs the frame into the slot. false = it does not fit, and the slot is untouched.
    bool WriteFrame(CamShared &slot, const Nv12Frame &frame);

    class CamMailbox
    {
    public:
        CamMailbox() = default;
        ~CamMailbox();
        CamMailbox(const CamMailbox &) = delete;
        CamMailbox &operator=(const CamMailbox &) = delete;

        bool Open();
        bool ReaderIsActive() const; // needs Open()
        bool Publish(const Nv12Frame &frame);

    private:
        HANDLE mapping_ = nullptr;
        std::atomic<CamShared *> slot_{nullptr}; // one thread opens it while another publishes
    };

} // namespace pd
