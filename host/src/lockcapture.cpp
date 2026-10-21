// PadDisplay by Nezucloud. Copyright (c) 2026 Nezucloud.
// SPDX-License-Identifier: MIT
#include "lockcapture.h"
#include "log.h"
#include "uacrelay.h" // kLockTexName, kLockFrameEvent

#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <sddl.h>
#include <cstdint>
#include <cstring>

namespace pd {

// SYSTEM and administrators only, like the helper pipe. The host runs elevated (administrator); the
// helper runs as SYSTEM. No one else can open the shared texture or the frame event.
static bool SecurityAttrs(SECURITY_ATTRIBUTES& sa, PSECURITY_DESCRIPTOR& sd) {
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:(A;;GA;;;SY)(A;;GA;;;BA)", SDDL_REVISION_1, &sd, nullptr))
        return false;
    sa = SECURITY_ATTRIBUTES{sizeof(sa), sd, FALSE};
    return true;
}

LockCaptureReceiver::~LockCaptureReceiver() { Stop(); }

// A D3D11 device on the GPU that RENDERS the lock / sign-in screen, and the GDI name of the display to
// capture there. Vendor-agnostic: whatever drives that display, an Intel/AMD iGPU or an NVIDIA/AMD dGPU.
// [excludeGdi] (the virtual monitor) is never chosen: an indirect-display adapter cannot share a capture
// texture, and the sign-in screen moves to a real display anyway. Prefers the primary real display (the one
// at 0,0 that is not excluded), else the first real attached display. false if none qualifies.
static bool CreateLockScreenDevice(ComPtr<ID3D11Device>& dev, ComPtr<ID3D11DeviceContext>& ctx, LUID& luid,
                                   const std::wstring& excludeGdi, std::wstring& gdiName) {
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return false;
    struct Cand { ComPtr<IDXGIAdapter1> adapter; LUID luid; std::wstring gdi; bool primary; };
    Cand best{};
    bool found = false;
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) == S_OK; ++i) {
        DXGI_ADAPTER_DESC1 ad{};
        adapter->GetDesc1(&ad);
        ComPtr<IDXGIOutput> out;
        for (UINT j = 0; adapter->EnumOutputs(j, &out) == S_OK; ++j) {
            DXGI_OUTPUT_DESC od{};
            out->GetDesc(&od);
            out.Reset();
            if (!od.AttachedToDesktop) continue;
            if (!excludeGdi.empty() && _wcsicmp(od.DeviceName, excludeGdi.c_str()) == 0) continue; // never the virtual monitor
            const bool primary = od.DesktopCoordinates.left == 0 && od.DesktopCoordinates.top == 0;
            if (!found || (primary && !best.primary)) {
                best = {adapter, ad.AdapterLuid, od.DeviceName, primary};
                found = true;
            }
        }
        adapter.Reset();
    }
    if (!found) return false;
    if (FAILED(D3D11CreateDevice(best.adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0,
                                 D3D11_SDK_VERSION, &dev, nullptr, &ctx)))
        return false;
    luid = best.luid;
    gdiName = best.gdi;
    return true;
}

bool LockCaptureReceiver::Start(ID3D11Device* device, int width, int height, const std::wstring& excludeGdi) {
    Stop();
    if (!device || width <= 0 || height <= 0) return false;

    // The encoder's adapter.
    ComPtr<IDXGIDevice> dxgiDev;
    ComPtr<IDXGIAdapter> encAdapter;
    DXGI_ADAPTER_DESC encDesc{};
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dxgiDev))) || FAILED(dxgiDev->GetAdapter(&encAdapter)) ||
        FAILED(encAdapter->GetDesc(&encDesc))) {
        LOGI("lockcap: could not read the encode adapter");
        return false;
    }
    const LUID encLuid = encDesc.AdapterLuid;

    // Put the shared texture on the GPU that renders the lock screen so the SYSTEM helper can DuplicateOutput
    // the primary output and write into it on one adapter. If that GPU differs from the encoder's (a hybrid
    // laptop: the sign-in screen is on the display GPU, encoding on the dGPU), bridge frames across adapters
    // in Next. If it is the same GPU - a single-GPU PC, whatever the vendor - or no primary output is
    // enumerable, keep the simple same-device path on the encoder adapter.
    ComPtr<ID3D11Device> lockDev;
    ComPtr<ID3D11DeviceContext> lockCtx;
    LUID lockLuid{};
    std::wstring lockGdi;
    ID3D11Device* texDevice = device;
    if (CreateLockScreenDevice(lockDev, lockCtx, lockLuid, excludeGdi, lockGdi) &&
        (lockLuid.LowPart != encLuid.LowPart || lockLuid.HighPart != encLuid.HighPart)) {
        bridge_ = true;
        lockDevice_ = lockDev;
        lockCtx_ = lockCtx;
        texDevice = lockDev.Get();
        luid_ = lockLuid;
        LOGI("lockcap: capturing the lock screen on %ls (real display, not the virtual monitor)", lockGdi.c_str());
    } else {
        bridge_ = false;
        luid_ = encLuid;
    }

    // Shared, keyed-mutex, NT-handle texture so the SYSTEM helper (another process) can open it by name on the
    // same adapter and write into it with cross-process synchronization.
    D3D11_TEXTURE2D_DESC td{};
    td.Width = UINT(width);
    td.Height = UINT(height);
    td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    td.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
    if (FAILED(texDevice->CreateTexture2D(&td, nullptr, &shared_))) {
        LOGI("lockcap: CreateTexture2D(shared) failed");
        Stop();
        return false;
    }
    if (FAILED(shared_.As(&mutex_))) { Stop(); return false; }

    // Bridge staging: shared(lock GPU) -> lockStaging_(CPU read, lock GPU) -> encStaging_(CPU write, encoder
    // GPU) -> dst(encoder GPU). Created only when the two GPUs differ.
    if (bridge_) {
        D3D11_TEXTURE2D_DESC sd = td;
        sd.BindFlags = 0;
        sd.MiscFlags = 0;
        sd.Usage = D3D11_USAGE_STAGING;
        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(lockDevice_->CreateTexture2D(&sd, nullptr, &lockStaging_))) { LOGI("lockcap: lock staging failed"); Stop(); return false; }
        sd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(device->CreateTexture2D(&sd, nullptr, &encStaging_))) { LOGI("lockcap: enc staging failed"); Stop(); return false; }
    }

    PSECURITY_DESCRIPTOR sd = nullptr;
    SECURITY_ATTRIBUTES sa{};
    if (!SecurityAttrs(sa, sd)) { Stop(); return false; }
    ComPtr<IDXGIResource1> res;
    HRESULT hr = shared_.As(&res);
    HANDLE sh = nullptr;
    if (SUCCEEDED(hr))
        hr = res->CreateSharedHandle(&sa, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, kLockTexName, &sh);
    if (sh) CloseHandle(sh); // only the NAME is used by the helper; the handle itself is not needed here
    if (FAILED(hr)) {
        LOGI("lockcap: CreateSharedHandle failed 0x%08lx", hr);
        if (sd) LocalFree(sd);
        Stop();
        return false;
    }

    frameEvent_ = CreateEventW(&sa, FALSE, FALSE, kLockFrameEvent); // auto-reset, not signalled
    if (sd) LocalFree(sd);
    if (!frameEvent_) {
        LOGI("lockcap: CreateEvent failed (%lu)", GetLastError());
        Stop();
        return false;
    }
    width_ = width;
    height_ = height;
    LOGI("lockcap: ready, shared %dx%d on lock-screen LUID %08lx:%08lx%s", width, height, luid_.HighPart, luid_.LowPart,
         bridge_ ? " (bridging to a separate encoder GPU)" : "");
    return true;
}

void LockCaptureReceiver::Stop() {
    if (frameEvent_) { CloseHandle(frameEvent_); frameEvent_ = nullptr; }
    mutex_.Reset();
    shared_.Reset();
    lockStaging_.Reset();
    encStaging_.Reset();
    lockCtx_.Reset();
    lockDevice_.Reset();
    bridge_ = false;
    width_ = height_ = 0;
}

bool LockCaptureReceiver::Next(ID3D11Device* device, unsigned timeoutMs, ID3D11Texture2D* dst, bool& fresh) {
    fresh = false;
    if (!shared_ || !mutex_ || !dst) return false;
    if (WaitForSingleObject(frameEvent_, timeoutMs) != WAIT_OBJECT_0) return false; // no new frame within the wait
    // Same key both sides (plain mutual exclusion, newest-frame-wins): the helper holds key 0 only while it
    // writes, so a short wait here is enough and a lagging consumer never stalls the producer.
    if (mutex_->AcquireSync(0, 200) != S_OK) return false;

    if (!bridge_) {
        // One GPU for both the lock screen and the encoder: a direct copy on the encoder device.
        ComPtr<ID3D11DeviceContext> ctx;
        device->GetImmediateContext(&ctx);
        ctx->CopyResource(dst, shared_.Get());
        mutex_->ReleaseSync(0);
        fresh = true;
        return true;
    }

    // Two GPUs: copy shared_ into a private staging on the lock GPU, release the mutex so the helper keeps
    // producing, then carry the pixels to the encoder GPU through system memory. Runs only while locked, so
    // the per-frame CPU copy never touches the normal capture/encode hot path.
    lockCtx_->CopyResource(lockStaging_.Get(), shared_.Get());
    mutex_->ReleaseSync(0);

    D3D11_MAPPED_SUBRESOURCE src{}, dstMap{};
    if (FAILED(lockCtx_->Map(lockStaging_.Get(), 0, D3D11_MAP_READ, 0, &src))) return false;
    ComPtr<ID3D11DeviceContext> encCtx;
    device->GetImmediateContext(&encCtx);
    if (FAILED(encCtx->Map(encStaging_.Get(), 0, D3D11_MAP_WRITE, 0, &dstMap))) { lockCtx_->Unmap(lockStaging_.Get(), 0); return false; }
    const size_t rowBytes = size_t(width_) * 4;
    for (int y = 0; y < height_; ++y)
        memcpy(static_cast<uint8_t*>(dstMap.pData) + size_t(y) * dstMap.RowPitch,
               static_cast<const uint8_t*>(src.pData) + size_t(y) * src.RowPitch, rowBytes);
    encCtx->Unmap(encStaging_.Get(), 0);
    lockCtx_->Unmap(lockStaging_.Get(), 0);
    encCtx->CopyResource(dst, encStaging_.Get());
    fresh = true;
    return true;
}

} // namespace pd
