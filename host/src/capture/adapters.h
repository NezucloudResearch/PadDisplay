// PadDisplay by Nezucloud. Copyright (c) 2026 Nezucloud.
// SPDX-License-Identifier: MIT
// Finding the GPU and the DXGI output of a monitor, and making a D3D11 device on it.
#pragma once
#include <d3d11.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <string>

namespace pd
{

    using Microsoft::WRL::ComPtr;

    struct CaptureTarget
    {
        ComPtr<IDXGIAdapter1> adapter;
        ComPtr<IDXGIOutput1> output;
        RECT desktopRect{}; // physical pixels in virtual-desktop space
        UINT vendorId = 0;
        std::wstring adapterName;
    };

    // Finds the DXGI output (and the adapter it is listed under) for a GDI source name.
    bool FindOutput(const std::wstring &gdiName, CaptureTarget &target);

    // The GPU that really renders this monitor, from Windows' display configuration: the monitor's adapter is the virtual display
    // driver's own, which carries the name of the GPU it renders on; the real adapter with that name is returned in target (adapter,
    // vendorId, adapterName; the rest is left alone). False when it cannot be told. DXGI cannot say: in only-screen mode it lists the
    // monitor under whichever adapter this process prefers, which may be the other GPU.
    bool FindRenderAdapter(const std::wstring &gdiName, CaptureTarget &target);
    // The first hardware adapter with this name (what the user chose), filling target the same way. For when FindRenderAdapter cannot say.
    bool FindAdapterByName(const std::wstring &name, CaptureTarget &target);

    // Creates a D3D11 device on the given adapter with video support (needed by both encoders).
    // gamingMode raises this process's GPU scheduling priority from high to realtime.
    bool CreateDevice(IDXGIAdapter1 *adapter, bool gamingMode, ComPtr<ID3D11Device> &device, ComPtr<ID3D11DeviceContext> &ctx);

} // namespace pd
