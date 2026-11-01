// PadDisplay by Nezucloud. Copyright (c) 2026 Nezucloud.
// SPDX-License-Identifier: MIT
#include "lockcapture.h"
#include "log.h"
#include "secure/uacrelay.h" // kLockTexName, kLockFrameEvent

#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <sddl.h>
#include <cstdint>
#include <cstring>

namespace pd
{
    namespace
    {

        constexpr UINT kKeyedMutexWaitMs = 200;

        // SYSTEM and administrators only, like the helper pipe. The host runs elevated (administrator); the
        // helper runs as SYSTEM. No one else can open the shared texture or the frame event.
        class HelperOnlyAccess
        {
        public:
            HelperOnlyAccess()
            {
                if (ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:(A;;GA;;;SY)(A;;GA;;;BA)", SDDL_REVISION_1, &sd_, nullptr))
                    attributes_ = SECURITY_ATTRIBUTES{sizeof(attributes_), sd_, FALSE};
            }
            ~HelperOnlyAccess()
            {
                if (sd_)
                    LocalFree(sd_);
            }
            HelperOnlyAccess(const HelperOnlyAccess &) = delete;
            HelperOnlyAccess &operator=(const HelperOnlyAccess &) = delete;

            bool Valid() const { return sd_ != nullptr; }
            SECURITY_ATTRIBUTES *Attributes() { return &attributes_; }

        private:
            PSECURITY_DESCRIPTOR sd_ = nullptr;
            SECURITY_ATTRIBUTES attributes_{};
        };

        struct LockScreenDisplay
        {
            ComPtr<IDXGIAdapter1> adapter;
            LUID luid{};
            std::wstring gdiName;
            bool primary = false;
        };

        // The display the lock screen shows on. Prefers the primary real display (the one at 0,0 that is not excluded),
        // else the first real attached display. false if none qualifies.
        bool FindLockScreenDisplay(const std::wstring &excludeGdi, LockScreenDisplay &best)
        {
            ComPtr<IDXGIFactory1> factory;
            if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
                return false;
            bool found = false;
            ComPtr<IDXGIAdapter1> adapter;
            for (UINT i = 0; factory->EnumAdapters1(i, &adapter) == S_OK; ++i)
            {
                DXGI_ADAPTER_DESC1 ad{};
                adapter->GetDesc1(&ad);
                ComPtr<IDXGIOutput> out;
                for (UINT j = 0; adapter->EnumOutputs(j, &out) == S_OK; ++j)
                {
                    DXGI_OUTPUT_DESC od{};
                    out->GetDesc(&od);
                    out.Reset();
                    if (!od.AttachedToDesktop)
                        continue;
                    if (!excludeGdi.empty() && _wcsicmp(od.DeviceName, excludeGdi.c_str()) == 0)
                        continue; // never the virtual monitor
                    const bool primary = od.DesktopCoordinates.left == 0 && od.DesktopCoordinates.top == 0;
                    if (!found || (primary && !best.primary))
                    {
                        best = {adapter, ad.AdapterLuid, od.DeviceName, primary};
                        found = true;
                    }
                }
                adapter.Reset();
            }
            return found;
        }

        bool AdapterLuidOf(ID3D11Device *device, LUID &luid)
        {
            ComPtr<IDXGIDevice> dxgiDevice;
            ComPtr<IDXGIAdapter> adapter;
            DXGI_ADAPTER_DESC desc{};
            if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dxgiDevice))) || FAILED(dxgiDevice->GetAdapter(&adapter)) || FAILED(adapter->GetDesc(&desc)))
                return false;
            luid = desc.AdapterLuid;
            return true;
        }

        bool SameLuid(const LUID &a, const LUID &b) { return a.LowPart == b.LowPart && a.HighPart == b.HighPart; }

    } // namespace

    LockCaptureReceiver::~LockCaptureReceiver() { Stop(); }

    bool LockCaptureReceiver::Start(ID3D11Device *device, int width, int height, const std::wstring &excludeGdi)
    {
        Stop();
        if (!device || width <= 0 || height <= 0)
            return false;
        LUID encoderLuid{};
        if (!AdapterLuidOf(device, encoderLuid))
        {
            LOGI("lockcap: could not read the encode adapter");
            return false;
        }
        ID3D11Device *textureDevice = ChooseTextureDevice(device, encoderLuid, excludeGdi);
        if (!CreateSharedTexture(textureDevice, width, height) || (bridge_ && !CreateBridgeStaging(device)) || !PublishToHelper())
        {
            Stop();
            return false;
        }
        width_ = width;
        height_ = height;
        LOGI("lockcap: ready, shared %dx%d on lock-screen LUID %08lx:%08lx%s", width, height, luid_.HighPart, luid_.LowPart,
             bridge_ ? " (bridging to a separate encoder GPU)" : "");
        return true;
    }

    // Puts the shared texture on the GPU that renders the lock screen, so the SYSTEM helper can DuplicateOutput the primary
    // output and write into it on one adapter. If that GPU differs from the encoder's (a hybrid laptop: the sign-in screen is on
    // the display GPU, encoding on the dGPU), frames are bridged across adapters in Next. If it is the same GPU - a single-GPU
    // PC, whatever the vendor - or no display qualifies, the simple same-device path on the encoder adapter stays.
    // Vendor-agnostic: whatever drives that display, an Intel/AMD iGPU or an NVIDIA/AMD dGPU. The virtual monitor is never
    // chosen: an indirect-display adapter cannot share a capture texture, and the sign-in screen moves to a real display anyway.
    ID3D11Device *LockCaptureReceiver::ChooseTextureDevice(ID3D11Device *encoderDevice, const LUID &encoderLuid, const std::wstring &excludeGdi)
    {
        bridge_ = false;
        luid_ = encoderLuid;
        LockScreenDisplay display;
        if (!FindLockScreenDisplay(excludeGdi, display) || SameLuid(display.luid, encoderLuid))
            return encoderDevice;
        if (FAILED(D3D11CreateDevice(display.adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &lockDevice_, nullptr, &lockCtx_)))
            return encoderDevice;
        bridge_ = true;
        luid_ = display.luid;
        LOGI("lockcap: capturing the lock screen on %ls (real display, not the virtual monitor)", display.gdiName.c_str());
        return lockDevice_.Get();
    }

    // Shared, keyed-mutex, NT-handle texture so the SYSTEM helper (another process) can open it by name on the
    // same adapter and write into it with cross-process synchronization.
    bool LockCaptureReceiver::CreateSharedTexture(ID3D11Device *device, int width, int height)
    {
        D3D11_TEXTURE2D_DESC td{};
        td.Width = UINT(width);
        td.Height = UINT(height);
        td.MipLevels = td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        td.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
        if (FAILED(device->CreateTexture2D(&td, nullptr, &shared_)))
        {
            LOGI("lockcap: CreateTexture2D(shared) failed");
            return false;
        }
        return SUCCEEDED(shared_.As(&mutex_));
    }

    // shared(lock GPU) -> lockStaging_(CPU read, lock GPU) -> encStaging_(CPU write, encoder GPU) -> dst(encoder GPU).
    bool LockCaptureReceiver::CreateBridgeStaging(ID3D11Device *encoderDevice)
    {
        D3D11_TEXTURE2D_DESC sd{};
        shared_->GetDesc(&sd);
        sd.BindFlags = 0;
        sd.MiscFlags = 0;
        sd.Usage = D3D11_USAGE_STAGING;
        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(lockDevice_->CreateTexture2D(&sd, nullptr, &lockStaging_)))
        {
            LOGI("lockcap: lock staging failed");
            return false;
        }
        sd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(encoderDevice->CreateTexture2D(&sd, nullptr, &encStaging_)))
        {
            LOGI("lockcap: enc staging failed");
            return false;
        }
        return true;
    }

    // Names the texture and the event, which is how the helper finds them.
    bool LockCaptureReceiver::PublishToHelper()
    {
        HelperOnlyAccess access;
        if (!access.Valid())
            return false;
        ComPtr<IDXGIResource1> resource;
        HRESULT hr = shared_.As(&resource);
        if (SUCCEEDED(hr))
            hr = resource->CreateSharedHandle(access.Attributes(), DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, kLockTexName, &sharedHandle_);
        if (FAILED(hr))
        {
            LOGI("lockcap: CreateSharedHandle failed 0x%08lx", hr);
            return false;
        }
        frameEvent_ = CreateEventW(access.Attributes(), FALSE, FALSE, kLockFrameEvent); // auto-reset, not signalled
        if (!frameEvent_)
        {
            LOGI("lockcap: CreateEvent failed (%lu)", GetLastError());
            return false;
        }
        return true;
    }

    void LockCaptureReceiver::Stop()
    {
        if (frameEvent_)
        {
            CloseHandle(frameEvent_);
            frameEvent_ = nullptr;
        }
        if (sharedHandle_)
        {
            CloseHandle(sharedHandle_);
            sharedHandle_ = nullptr;
        }
        mutex_.Reset();
        shared_.Reset();
        lockStaging_.Reset();
        encStaging_.Reset();
        lockCtx_.Reset();
        lockDevice_.Reset();
        bridge_ = false;
        width_ = height_ = 0;
    }

    bool LockCaptureReceiver::Next(ID3D11Device *device, unsigned timeoutMs, ID3D11Texture2D *dst, bool &fresh)
    {
        fresh = false;
        if (!shared_ || !mutex_ || !dst)
            return false;
        if (WaitForSingleObject(frameEvent_, timeoutMs) != WAIT_OBJECT_0)
            return false; // no new frame within the wait
        // Same key both sides (plain mutual exclusion, newest-frame-wins): the helper holds key 0 only while it
        // writes, so a short wait here is enough and a lagging consumer never stalls the producer.
        if (mutex_->AcquireSync(0, kKeyedMutexWaitMs) != S_OK)
            return false;

        if (bridge_)
        {
            fresh = CopyAcrossGpus(device, dst);
            return fresh;
        }
        // One GPU for both the lock screen and the encoder: a direct copy on the encoder device.
        ComPtr<ID3D11DeviceContext> ctx;
        device->GetImmediateContext(&ctx);
        ctx->CopyResource(dst, shared_.Get());
        mutex_->ReleaseSync(0);
        fresh = true;
        return true;
    }

    // Copies shared_ into a private staging on the lock GPU, releases the mutex so the helper keeps producing, then carries
    // the pixels to the encoder GPU through system memory. Runs only while locked, so the per-frame CPU copy never touches the
    // normal capture/encode hot path.
    bool LockCaptureReceiver::CopyAcrossGpus(ID3D11Device *device, ID3D11Texture2D *dst)
    {
        lockCtx_->CopyResource(lockStaging_.Get(), shared_.Get());
        mutex_->ReleaseSync(0);

        D3D11_MAPPED_SUBRESOURCE src{}, dstMap{};
        if (FAILED(lockCtx_->Map(lockStaging_.Get(), 0, D3D11_MAP_READ, 0, &src)))
            return false;
        ComPtr<ID3D11DeviceContext> encCtx;
        device->GetImmediateContext(&encCtx);
        if (FAILED(encCtx->Map(encStaging_.Get(), 0, D3D11_MAP_WRITE, 0, &dstMap)))
        {
            lockCtx_->Unmap(lockStaging_.Get(), 0);
            return false;
        }
        const size_t rowBytes = size_t(width_) * 4;
        for (int y = 0; y < height_; ++y)
            memcpy(static_cast<uint8_t *>(dstMap.pData) + size_t(y) * dstMap.RowPitch,
                   static_cast<const uint8_t *>(src.pData) + size_t(y) * src.RowPitch, rowBytes);
        encCtx->Unmap(encStaging_.Get(), 0);
        lockCtx_->Unmap(lockStaging_.Get(), 0);
        encCtx->CopyResource(dst, encStaging_.Get());
        return true;
    }

} // namespace pd
