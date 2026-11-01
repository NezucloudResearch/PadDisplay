// PadDisplay by Nezucloud. Copyright (c) 2026 Nezucloud.
// SPDX-License-Identifier: MIT
#include "capture_wgc.h"
#include "log.h"

// The capture interop interfaces are declared from Windows 10 2004 on; the rest of the host targets Windows 10 1809 headers.
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
#include <utility>

namespace pd
{
    namespace
    {

        namespace wgc = winrt::Windows::Graphics::Capture;
        using EventArgs = winrt::Windows::Foundation::IInspectable;
        using Microsoft::WRL::ComPtr;
        using winrt::Windows::Graphics::DirectX::DirectXPixelFormat;
        using winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice;

        constexpr int32_t kPoolFrames = 3; // one being copied, one waiting to be taken, one for Windows to fill

        struct MonitorSearch
        {
            const std::wstring *gdiName;
            HMONITOR monitor = nullptr;
        };

        HMONITOR FindMonitor(const std::wstring &gdiName)
        {
            MonitorSearch search{&gdiName};
            EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR m, HDC, LPRECT, LPARAM p) -> BOOL
                                {
        auto* s = reinterpret_cast<MonitorSearch*>(p);
        MONITORINFOEXW mi{};
        mi.cbSize = sizeof(mi);
        if (GetMonitorInfoW(m, &mi) && _wcsicmp(mi.szDevice, s->gdiName->c_str()) == 0) s->monitor = m;
        return TRUE; }, reinterpret_cast<LPARAM>(&search));
            return search.monitor;
        }

        wgc::GraphicsCaptureItem CaptureItemFor(HMONITOR monitor)
        {
            wgc::GraphicsCaptureItem item{nullptr};
            auto interop = winrt::get_activation_factory<wgc::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
            winrt::check_hresult(interop->CreateForMonitor(monitor, winrt::guid_of<wgc::GraphicsCaptureItem>(), winrt::put_abi(item)));
            return item;
        }

        IDirect3DDevice WinrtDeviceFor(ID3D11Device *device)
        {
            ComPtr<IDXGIDevice> dxgiDevice;
            winrt::check_hresult(device->QueryInterface(IID_PPV_ARGS(&dxgiDevice)));
            winrt::com_ptr<::IInspectable> inspectable;
            winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgiDevice.Get(), inspectable.put()));
            return inspectable.as<IDirect3DDevice>();
        }

        ComPtr<ID3D11Texture2D> TextureOf(const wgc::Direct3D11CaptureFrame &frame)
        {
            auto access = frame.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
            ComPtr<ID3D11Texture2D> texture;
            winrt::check_hresult(access->GetInterface(IID_PPV_ARGS(&texture)));
            return texture;
        }

        // For a call that only newer versions of Windows have. false = this one does not.
        template <class Call>
        bool IfAvailable(Call &&call)
        {
            try
            {
                call();
                return true;
            }
            catch (winrt::hresult_error const &)
            {
                return false;
            }
        }

        template <class Closable>
        void CloseQuietly(Closable &closable)
        {
            if (closable)
                IfAvailable([&]
                            { closable.Close(); });
        }

        // What Windows' capture callbacks share with Next. They run on a thread pool, and closing the session does not wait for one
        // that is under way, so they keep this alive themselves: it can outlive the WgcSource by a moment.
        class FrameSlot
        {
        public:
            FrameSlot() { winrt::check_bool(wake_ != nullptr); }
            ~FrameSlot() { CloseHandle(wake_); }
            FrameSlot(const FrameSlot &) = delete;
            FrameSlot &operator=(const FrameSlot &) = delete;

            void Offer(wgc::Direct3D11CaptureFrame frame)
            {
                {
                    std::lock_guard lock(mutex_);
                    std::swap(pending_, frame); // an older frame nobody took: newest state wins
                }
                SetEvent(wake_);
            } // the older frame goes back to the pool here, not under the lock Next takes

            void OnItemClosed()
            {
                itemClosed_ = true;
                SetEvent(wake_);
            }

            void Wait(UINT timeoutMs) const { WaitForSingleObject(wake_, timeoutMs); }
            bool ItemClosed() const { return itemClosed_; }

            wgc::Direct3D11CaptureFrame Take()
            {
                std::lock_guard lock(mutex_);
                return std::exchange(pending_, nullptr);
            }

        private:
            std::mutex mutex_;
            wgc::Direct3D11CaptureFrame pending_{nullptr}; // the newest frame not taken yet
            HANDLE wake_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            std::atomic<bool> itemClosed_{false};
        };

    } // namespace

    struct WgcSource::Impl
    {
        wgc::GraphicsCaptureItem item{nullptr};
        wgc::Direct3D11CaptureFramePool pool{nullptr};
        wgc::GraphicsCaptureSession session{nullptr};
        winrt::event_token arrived{}, closed{};
        std::shared_ptr<FrameSlot> slot = std::make_shared<FrameSlot>();
        int width = 0, height = 0;

        ~Impl()
        {
            if (pool)
                pool.FrameArrived(arrived);
            if (item)
                item.Closed(closed);
            CloseQuietly(session);
            CloseQuietly(pool);
        }

        void Open(HMONITOR monitor, ID3D11Device *device)
        {
            item = CaptureItemFor(monitor);
            const auto size = item.Size();
            width = size.Width;
            height = size.Height;
            pool = wgc::Direct3D11CaptureFramePool::CreateFreeThreaded(WinrtDeviceFor(device), DirectXPixelFormat::B8G8R8A8UIntNormalized, kPoolFrames, size);
            arrived = pool.FrameArrived([slot = slot](wgc::Direct3D11CaptureFramePool const &source, EventArgs const &)
                                        {
            auto frame = source.TryGetNextFrame();
            if (frame) slot->Offer(std::move(frame)); });
            closed = item.Closed([slot = slot](wgc::GraphicsCaptureItem const &, EventArgs const &)
                                 { slot->OnItemClosed(); });
            session = pool.CreateCaptureSession(item);
        }

        void Start(bool cursor, unsigned minIntervalMs)
        {
            IfAvailable([&]
                        { session.IsCursorCaptureEnabled(cursor); });
            // The yellow "this screen is being captured" frame is drawn on the PC's virtual monitor only (never into the frames).
            IfAvailable([&]
                        { session.IsBorderRequired(false); });
            const bool paced = IfAvailable([&]
                                           { session.MinUpdateInterval(std::chrono::milliseconds(minIntervalMs)); });
            session.StartCapture();
            LOGI("wgc: capturing %dx%d%s", width, height, paced ? "" : " (this Windows caps the rate at about 60 fps)");
        }
    };

    WgcSource::WgcSource(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
    WgcSource::~WgcSource() = default;

    int WgcSource::Width() const { return impl_->width; }
    int WgcSource::Height() const { return impl_->height; }

    std::unique_ptr<WgcSource> WgcSource::Create(ID3D11Device *device, const std::wstring &gdiName, bool cursor, unsigned minIntervalMs)
    {
        IfAvailable([]
                    { winrt::init_apartment(winrt::apartment_type::multi_threaded); }); // already initialised on most threads: that is fine
        try
        {
            HMONITOR monitor = FindMonitor(gdiName);
            if (!monitor)
            {
                LOGI("wgc: monitor %ls not found", gdiName.c_str());
                return nullptr;
            }
            auto impl = std::make_unique<Impl>();
            impl->Open(monitor, device);
            impl->Start(cursor, minIntervalMs);
            return std::unique_ptr<WgcSource>(new WgcSource(std::move(impl)));
        }
        catch (winrt::hresult_error const &e)
        {
            LOGI("wgc: not available (0x%08x)", unsigned(e.code().value));
        }
        catch (...)
        {
            LOGI("wgc: not available");
        }
        return nullptr;
    }

    WgcSource::Result WgcSource::Next(UINT timeoutMs, ID3D11DeviceContext *ctx, ID3D11Texture2D *dst)
    {
        FrameSlot &slot = *impl_->slot;
        slot.Wait(timeoutMs); // woken by the next frame, not a poll
        if (slot.ItemClosed())
            return Result::Lost;
        const auto frame = slot.Take();
        if (!frame)
            return Result::Timeout;
        try
        {
            const auto content = frame.ContentSize();
            if (content.Width != impl_->width || content.Height != impl_->height)
                return Result::Lost; // the monitor changed mode
            ctx->CopyResource(dst, TextureOf(frame).Get());
            return Result::Frame;
        }
        catch (winrt::hresult_error const &e)
        {
            LOGI("wgc: frame failed (0x%08x)", unsigned(e.code().value));
            return Result::Lost;
        }
    }

} // namespace pd
