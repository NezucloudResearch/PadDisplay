// PadDisplay by Nezucloud. Copyright (c) 2026 Nezucloud.
// SPDX-License-Identifier: MIT
// The mailbox (camshared.h) from the virtual camera's side of the glass. Runs on the stream's pump thread only.
#pragma once
#include "camera/camshared.h"

#include <windows.h>
#include <vector>

namespace pd
{

    // A frame this old means the tablet stopped. nowMs and writeTick come from the same clock, read a moment apart.
    bool FrameIsFresh(const CamShared &shared, uint64_t nowMs);

    // The newest complete frame seen in the mailbox, kept so that a torn read can show the previous one again.
    class FrameCopy
    {
    public:
        void TakeNewest(CamShared *shared);
        bool HasFrame() const { return width_ != 0; }
        // Copies (or nearest-neighbour scales when the sizes differ) the frame into dst: NV12, pitch bytes per row, UV plane at h * pitch.
        void Blit(BYTE *dst, LONG pitch, UINT32 w, UINT32 h) const;

    private:
        std::vector<BYTE> front_, back_;
        UINT32 width_ = 0, height_ = 0;
    };

    // Dark grey with a bar sweeping across, so a frozen picture is never mistaken for a live one.
    void DrawNoSignal(BYTE *dst, LONG pitch, UINT32 w, UINT32 h, uint64_t nowMs);

    class Mailbox
    {
    public:
        ~Mailbox();
        // Fills dst with the newest frame. false = no signal.
        bool Read(BYTE *dst, LONG pitch, UINT32 w, UINT32 h);

    private:
        bool Open();

        HANDLE map_ = nullptr;
        CamShared *view_ = nullptr;
        FrameCopy frame_;
    };

} // namespace pd
