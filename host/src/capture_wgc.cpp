// PadDisplay by Nezucloud. Copyright (c) 2026 Nezucloud.
// SPDX-License-Identifier: MIT
#include "capture_wgc.h"
#include "log.h"

// The capture interop interfaces are declared from Windows 10 2004 on; the rest of the host targets Windows 10 1607 headers.
#undef NTDDI_VERSION
#define NTDDI_VERSION 0x0A00000B

#include <dxgi.h>
#include <inspectable.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <wrl/client.h>
#include <atomic>
#include <chrono>
#include <mutex>

namespace pd {
namespace {

namespace wgc = winrt::Windows::Graphics::Capture;

struct MonitorSearch {
    const std::wstring* gdiName;
    HMONITOR monitor = nullptr;
};

HMONITOR FindMonitor(const std::wstring& gdiName) {
    MonitorSearch search{&gdiName};
    EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR m, HDC, LPRECT, LPARAM p) -> BOOL {
        auto* s = reinterpret_cast<MonitorSearch*>(p);
        MONITORINFOEXW mi{};
        mi.cbSize = sizeof(mi);
        if (GetMonitorInfoW(m, &mi) && _wcsicmp(mi.szDevice, s->gdiName->c_str()) == 0) s->monitor = m;
        return TRUE;
    }, reinterpret_cast<LPARAM>(&search));
    return search.monitor;
}

} // namespace

struct WgcSource::Impl {
    wgc::GraphicsCaptureItem item{nullptr};
    wgc::Direct3D11CaptureFramePool pool{nullptr};
    wgc::GraphicsCaptureSession session{nullptr};
    winrt::event_token arrived{}, closed{};
    std::mutex mutex;
    wgc::Direct3D11CaptureFrame pending{nullptr}; // the newest frame not taken yet; an older one is given back to the pool
    HANDLE frameEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    std::atomic<bool> itemClosed{false};
    int width = 0, height = 0;

    ~Impl() {
        try {
            if (pool) pool.FrameArrived(arrived);
            if (item) item.Closed(closed);
            if (session) session.Close();
            if (pool) pool.Close();
        } catch (...) {
        }
        if (frameEvent) CloseHandle(frameEvent);
    }
};

WgcSource::WgcSource(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
WgcSource::~WgcSource() = default;

std::unique_ptr<WgcSource> WgcSource::Create(ID3D11Device* device, const std::wstring& gdiName, bool cursor, unsigned minIntervalMs,
                                             int& width, int& height) {
    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded); // already initialised on most threads: that is fine
    } catch (...) {
    }
    try {
        HMONITOR monitor = FindMonitor(gdiName);
        if (!monitor) {
            LOGI("wgc: monitor %ls not found", gdiName.c_str());
            return nullptr;
        }
        auto impl = std::make_unique<Impl>();
        auto interop = winrt::get_activation_factory<wgc::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
        winrt::check_hresult(interop->CreateForMonitor(monitor, winrt::guid_of<wgc::GraphicsCaptureItem>(), winrt::put_abi(impl->item)));

        Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice;
        winrt::check_hresult(device->QueryInterface(IID_PPV_ARGS(&dxgiDevice)));
        winrt::com_ptr<::IInspectable> inspectable;
        winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgiDevice.Get(), inspectable.put()));
        auto winrtDevice = inspectable.as<winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice>();

        const auto size = impl->item.Size();
        impl->width = size.Width;
        impl->height = size.Height;
        impl->pool = wgc::Direct3D11CaptureFramePool::CreateFreeThreaded(
            winrtDevice, winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized, 3, size);
        Impl* raw = impl.get();
        impl->arrived = impl->pool.FrameArrived([raw](wgc::Direct3D11CaptureFramePool const& p, winrt::Windows::Foundation::IInspectable const&) {
            auto frame = p.TryGetNextFrame();
            if (!frame) return;
            {
                std::lock_guard lock(raw->mutex);
                raw->pending = frame; // replaces (and so releases) an older frame nobody took: newest state wins
            }
            SetEvent(raw->frameEvent);
        });
        impl->closed = impl->item.Closed([raw](wgc::GraphicsCaptureItem const&, winrt::Windows::Foundation::IInspectable const&) {
            raw->itemClosed = true;
            SetEvent(raw->frameEvent);
        });
        impl->session = impl->pool.CreateCaptureSession(impl->item);
        try { impl->session.IsCursorCaptureEnabled(cursor); } catch (...) {}
        // The yellow "this screen is being captured" frame is drawn on the PC's virtual monitor only (never into the frames).
        try { impl->session.IsBorderRequired(false); } catch (...) {}
        bool paced = false;
        if (minIntervalMs > 0) {
            try {
                impl->session.MinUpdateInterval(std::chrono::duration<int64_t, std::ratio<1, 10000000>>(int64_t(minIntervalMs) * 10000));
                paced = true;
            } catch (...) {
            }
        }
        impl->session.StartCapture();
        LOGI("wgc: capturing %dx%d%s", impl->width, impl->height, paced ? "" : " (this Windows caps the rate at about 60 fps)");
        width = impl->width;
        height = impl->height;
        return std::unique_ptr<WgcSource>(new WgcSource(std::move(impl)));
    } catch (winrt::hresult_error const& e) {
        LOGI("wgc: not available (0x%08x)", unsigned(e.code().value));
    } catch (...) {
        LOGI("wgc: not available");
    }
    return nullptr;
}

WgcSource::Result WgcSource::Next(UINT timeoutMs, ID3D11DeviceContext* ctx, ID3D11Texture2D* dst) {
    Impl& s = *impl_;
    WaitForSingleObject(s.frameEvent, timeoutMs); // woken by the next frame, not a poll
    if (s.itemClosed) return Result::Lost;
    wgc::Direct3D11CaptureFrame frame{nullptr};
    {
        std::lock_guard lock(s.mutex);
        frame = std::move(s.pending);
        s.pending = nullptr;
    }
    if (!frame) return Result::Timeout;
    try {
        const auto content = frame.ContentSize();
        if (content.Width != s.width || content.Height != s.height) return Result::Lost; // the monitor changed mode
        auto access = frame.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        winrt::check_hresult(access->GetInterface(IID_PPV_ARGS(&texture)));
        ctx->CopyResource(dst, texture.Get());
        return Result::Frame;
    } catch (winrt::hresult_error const& e) {
        LOGI("wgc: frame failed (0x%08x)", unsigned(e.code().value));
        return Result::Lost;
    }
}

} // namespace pd
