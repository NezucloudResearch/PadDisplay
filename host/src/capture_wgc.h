// PadDisplay by Nezucloud. Copyright (c) 2026 Nezucloud.
// SPDX-License-Identifier: MIT
// Windows.Graphics.Capture of one monitor. The second way to capture the virtual monitor, next to DXGI Desktop Duplication: it does
// not care which GPU the process prefers, so it works when the monitor is rendered by a GPU other than the one Desktop Duplication
// would look at (see Session::SetupPipeline). Whole frames only, no dirty rectangles: Desktop Duplication stays the first choice.
#pragma once
#include <d3d11.h>
#include <memory>
#include <string>

namespace pd {

class WgcSource {
public:
    enum class Result { Frame, Timeout, Lost };

    // device must be on the GPU that renders the monitor (a device on another GPU works, with a copy across the bus per frame).
    // cursor: let Windows draw the pointer into the frames. minIntervalMs: the shortest time between two frames (Windows 11 24H2
    // and later; without it capture stops at about 60 frames a second).
    // Returns null when Windows.Graphics.Capture is not there (before Windows 10 2004), the monitor was not found, or a call failed.
    static std::unique_ptr<WgcSource> Create(ID3D11Device* device, const std::wstring& gdiName, bool cursor, unsigned minIntervalMs,
                                             int& width, int& height);
    ~WgcSource();

    // Waits (on an event) up to timeoutMs for a newer frame and copies it into dst, which is as big as the monitor and B8G8R8A8.
    // Lost: the monitor went away or changed size, so the capture has to be built again.
    Result Next(UINT timeoutMs, ID3D11DeviceContext* ctx, ID3D11Texture2D* dst);

private:
    struct Impl;
    explicit WgcSource(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

} // namespace pd
