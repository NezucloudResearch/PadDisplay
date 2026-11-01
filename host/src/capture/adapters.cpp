// PadDisplay by Nezucloud. Copyright (c) 2026 Nezucloud.
// SPDX-License-Identifier: MIT
#include "adapters.h"
#include "log.h"

#include <vector>

namespace pd
{
    namespace
    {

        void NameTarget(CaptureTarget &target, const ComPtr<IDXGIAdapter1> &adapter, const DXGI_ADAPTER_DESC1 &desc)
        {
            target.adapter = adapter;
            target.vendorId = desc.VendorId;
            target.adapterName = desc.Description;
        }

        bool SameLuid(const LUID &a, const LUID &b) { return a.LowPart == b.LowPart && a.HighPart == b.HighPart; }

        // The adapter Windows' display configuration has this monitor on.
        bool FindMonitorAdapterLuid(const std::wstring &gdiName, LUID &luid)
        {
            UINT32 pathCount = 0, modeCount = 0;
            if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount) != ERROR_SUCCESS)
                return false;
            std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
            std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
            if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(), &modeCount, modes.data(), nullptr) != ERROR_SUCCESS)
                return false;
            for (const auto &path : paths)
            {
                DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
                source.header = {DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME, sizeof(source), path.sourceInfo.adapterId, path.sourceInfo.id};
                if (DisplayConfigGetDeviceInfo(&source.header) == ERROR_SUCCESS && _wcsicmp(source.viewGdiDeviceName, gdiName.c_str()) == 0)
                {
                    luid = path.sourceInfo.adapterId;
                    return true;
                }
            }
            return false;
        }

        struct Adapter
        {
            ComPtr<IDXGIAdapter1> handle;
            DXGI_ADAPTER_DESC1 desc;
        };

        std::vector<Adapter> ListAdapters()
        {
            std::vector<Adapter> adapters;
            ComPtr<IDXGIFactory1> factory;
            if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
                return adapters;
            ComPtr<IDXGIAdapter1> adapter;
            for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i)
            {
                Adapter a{adapter, {}};
                adapter->GetDesc1(&a.desc);
                adapters.push_back(a);
            }
            return adapters;
        }

        // D3DKMTSetProcessSchedulingPriorityClass (gdi32): lets our copy/encode work preempt normal GPU work.
        // High is enough next to desktop apps, but a game that keeps the GPU at 100% still starves it
        // (a frame took 30-40 ms instead of 6). Gaming mode asks for realtime, as Sunshine does. It is
        // opt-in: with realtime the NVIDIA encoder is known to freeze when video memory runs out.
        void RaiseGpuPriority(bool realtime)
        {
            using Fn = LONG(APIENTRY *)(HANDLE, int);
            const int kHigh = 4, kRealtime = 5; // D3DKMT_SCHEDULINGPRIORITYCLASS_HIGH / _REALTIME
            static int applied = 0;
            int want = realtime ? kRealtime : kHigh;
            if (applied == want)
                return;
            applied = want;
            auto fn = reinterpret_cast<Fn>(GetProcAddress(GetModuleHandleW(L"gdi32.dll"), "D3DKMTSetProcessSchedulingPriorityClass"));
            if (!fn)
                return;
            LONG st = fn(GetCurrentProcess(), want);
            LOGI("gpu scheduling priority %s -> 0x%08lx", realtime ? "REALTIME (gaming mode)" : "HIGH", st);
            if (st != 0 && realtime)
            { // not allowed on this system
                applied = kHigh;
                st = fn(GetCurrentProcess(), kHigh);
                LOGI("gpu scheduling priority HIGH -> 0x%08lx", st);
            }
        }

    } // namespace

    bool FindOutput(const std::wstring &gdiName, CaptureTarget &target)
    {
        ComPtr<IDXGIFactory1> factory;
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
            return false;
        ComPtr<IDXGIAdapter1> adapter;
        for (UINT a = 0; factory->EnumAdapters1(a, &adapter) != DXGI_ERROR_NOT_FOUND; ++a)
        {
            ComPtr<IDXGIOutput> out;
            for (UINT o = 0; adapter->EnumOutputs(o, &out) != DXGI_ERROR_NOT_FOUND; ++o)
            {
                DXGI_OUTPUT_DESC od;
                out->GetDesc(&od);
                if (_wcsicmp(od.DeviceName, gdiName.c_str()) != 0)
                    continue;
                DXGI_ADAPTER_DESC1 ad;
                adapter->GetDesc1(&ad);
                NameTarget(target, adapter, ad);
                out.As(&target.output);
                target.desktopRect = od.DesktopCoordinates;
                return target.output != nullptr;
            }
        }
        return false;
    }

    bool FindRenderAdapter(const std::wstring &gdiName, CaptureTarget &target)
    {
        LUID monitorAdapter{};
        if (!FindMonitorAdapterLuid(gdiName, monitorAdapter))
            return false;
        const std::vector<Adapter> adapters = ListAdapters();
        // The monitor's adapter is the virtual display driver's, named after the GPU it renders on...
        const Adapter *driverAdapter = nullptr;
        for (const Adapter &a : adapters)
        {
            if (SameLuid(a.desc.AdapterLuid, monitorAdapter))
            {
                driverAdapter = &a;
                break;
            }
        }
        if (!driverAdapter)
            return false;
        // ...and the real adapter of that name is the one to put a device on (the driver's own one is only its stand-in).
        for (const Adapter &a : adapters)
        {
            if (!SameLuid(a.desc.AdapterLuid, monitorAdapter) && !(a.desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) && wcscmp(a.desc.Description, driverAdapter->desc.Description) == 0)
            {
                NameTarget(target, a.handle, a.desc);
                return true;
            }
        }
        NameTarget(target, driverAdapter->handle, driverAdapter->desc);
        return true;
    }

    bool FindAdapterByName(const std::wstring &name, CaptureTarget &target)
    {
        for (const Adapter &a : ListAdapters())
        {
            if ((a.desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) || name != a.desc.Description)
                continue;
            NameTarget(target, a.handle, a.desc);
            return true;
        }
        return false;
    }

    bool CreateDevice(IDXGIAdapter1 *adapter, bool gamingMode, ComPtr<ID3D11Device> &device, ComPtr<ID3D11DeviceContext> &ctx)
    {
        RaiseGpuPriority(gamingMode);
        const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
        UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
        HRESULT hr = D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags, levels, 2, D3D11_SDK_VERSION,
                                       &device, nullptr, &ctx);
        if (FAILED(hr))
        {
            LOGI("D3D11CreateDevice failed 0x%08lx", hr);
            return false;
        }
        ComPtr<ID3D10Multithread> mt;
        if (SUCCEEDED(ctx.As(&mt)))
            mt->SetMultithreadProtected(TRUE);
        ComPtr<IDXGIDevice> dxgiDev;
        if (SUCCEEDED(device.As(&dxgiDev)))
            dxgiDev->SetGPUThreadPriority(7); // best effort, may need privileges
        return true;
    }

} // namespace pd
