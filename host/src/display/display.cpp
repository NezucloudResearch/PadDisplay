// PadDisplay by Nezucloud. Copyright (c) 2026 Nezucloud.
// SPDX-License-Identifier: MIT
#include "display.h"
#include "log.h"
#include "settings.h"

#include <dxgi1_6.h>
#include <wrl/client.h>
#include <algorithm>
#include <cstdio>
#include <cwctype>

using Microsoft::WRL::ComPtr;

namespace pd
{
    static std::wstring Lower(std::wstring s)
    {
        std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c)
                       { return wchar_t(std::towlower(c)); });
        return s;
    }

    bool IsVirtualAdapter(const DISPLAY_DEVICEW &dd)
    {
        std::wstring desc = Lower(dd.DeviceString), id = Lower(dd.DeviceID);
        return desc.find(L"virtual display") != std::wstring::npos || id.find(L"mttvdd") != std::wstring::npos ||
               id.find(L"root\\display") != std::wstring::npos || id.find(L"iddsampledriver") != std::wstring::npos;
    }

    static bool HasMonitor(const std::wstring &gdiName)
    {
        DISPLAY_DEVICEW mon{sizeof(mon)};
        return EnumDisplayDevicesW(gdiName.c_str(), 0, &mon, 0) != FALSE;
    }

    bool FindVirtualDisplay(const std::wstring &overrideName, VirtualDisplay &out)
    {
        VirtualDisplay best;
        bool found = false;
        DISPLAY_DEVICEW dd{sizeof(dd)};
        for (DWORD i = 0; EnumDisplayDevicesW(nullptr, i, &dd, 0); ++i, dd = {sizeof(dd)})
        {
            bool match = overrideName.empty() ? IsVirtualAdapter(dd) : _wcsicmp(dd.DeviceName, overrideName.c_str()) == 0;
            if (!match)
                continue;
            bool attached = (dd.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP) != 0;
            if (!attached && !HasMonitor(dd.DeviceName))
                continue;
            // Prefer a source that is already attached to the desktop.
            if (!found || (attached && !best.attached))
            {
                best = {dd.DeviceName, dd.DeviceString, attached};
                found = true;
            }
        }
        if (found)
            out = best;
        return found;
    }

    std::vector<DisplayMode> ListModes(const std::wstring &gdiName)
    {
        std::vector<DisplayMode> modes;
        DEVMODEW dm{};
        dm.dmSize = sizeof(dm);
        for (DWORD i = 0; EnumDisplaySettingsExW(gdiName.c_str(), i, &dm, 0); ++i)
        {
            if (dm.dmBitsPerPel != 32)
                continue;
            DisplayMode m{int(dm.dmPelsWidth), int(dm.dmPelsHeight), int(dm.dmDisplayFrequency)};
            if (std::none_of(modes.begin(), modes.end(), [&](auto &x)
                             { return x.width == m.width && x.height == m.height && x.hz == m.hz; }))
                modes.push_back(m);
        }
        return modes;
    }

    static bool CurrentMode(const std::wstring &gdiName, DisplayMode &m)
    {
        DEVMODEW dm{};
        dm.dmSize = sizeof(dm);
        if (!EnumDisplaySettingsExW(gdiName.c_str(), ENUM_CURRENT_SETTINGS, &dm, 0) || dm.dmPelsWidth == 0)
            return false;
        m = {int(dm.dmPelsWidth), int(dm.dmPelsHeight), int(dm.dmDisplayFrequency)};
        return true;
    }

    // Rank refresh rates: exact match first, then the highest below, then the lowest above.
    static int HzScore(int hz, int want)
    {
        if (hz == want)
            return 1000000;
        if (hz < want)
            return 500000 + hz;
        return 250000 - hz;
    }

    DisplayMode PickMode(const std::vector<DisplayMode> &modes, DisplayMode want, bool matchResolution, const DisplayMode *current)
    {
        const DisplayMode *best = nullptr;
        auto consider = [&](auto pred)
        {
            for (auto &m : modes)
                if (pred(m) && (!best || HzScore(m.hz, want.hz) > HzScore(best->hz, want.hz)))
                    best = &m;
        };
        if (matchResolution)
            consider([&](auto &m)
                     { return m.width == want.width && m.height == want.height; });
        if (!best && current)
            consider([&](auto &m)
                     { return m.width == current->width && m.height == current->height; });
        if (!best)
        {
            // Largest mode that fits the client screen, otherwise 1920x1080-ish.
            long long bestArea = -1;
            for (auto &m : modes)
            {
                long long area = 1LL * m.width * m.height;
                bool fits = m.width <= std::max(want.width, 1920) && m.height <= std::max(want.height, 1080);
                if (fits && (area > bestArea || (area == bestArea && HzScore(m.hz, want.hz) > HzScore(best->hz, want.hz))))
                {
                    best = &m;
                    bestArea = area;
                }
            }
        }
        return best ? *best : (current ? *current : want);
    }

    static RECT DesktopBounds()
    {
        RECT u{0, 0, 0, 0};
        EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR, HDC, LPRECT r, LPARAM p) -> BOOL
                            {
        UnionRect(reinterpret_cast<RECT*>(p), reinterpret_cast<RECT*>(p), r);
        return TRUE; }, reinterpret_cast<LPARAM>(&u));
        return u;
    }

    static bool IsAttached(const std::wstring &gdiName)
    {
        DISPLAY_DEVICEW dd{sizeof(dd)};
        for (DWORD i = 0; EnumDisplayDevicesW(nullptr, i, &dd, 0); ++i, dd = {sizeof(dd)})
            if (_wcsicmp(dd.DeviceName, gdiName.c_str()) == 0)
                return (dd.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP) != 0;
        return false;
    }

    bool CurrentPlacement(const std::wstring &gdiName, VirtualPlacement &out)
    {
        if (!IsAttached(gdiName))
            return false;
        DEVMODEW dm{};
        dm.dmSize = sizeof(dm);
        if (!EnumDisplaySettingsExW(gdiName.c_str(), ENUM_CURRENT_SETTINGS, &dm, 0) || dm.dmPelsWidth == 0)
            return false;
        out = {dm.dmPosition.x, dm.dmPosition.y, {int(dm.dmPelsWidth), int(dm.dmPelsHeight), int(dm.dmDisplayFrequency)}};
        return true;
    }

    static std::wstring PlacementPath() { return AppDataDir() + L"\\virtual-monitor.txt"; }

    void SaveVirtualPlacement(int tabletW, int tabletH, const VirtualPlacement &p)
    {
        // One line per tablet: "tabletW tabletH x y width height hz"; replace this tablet's line.
        std::vector<std::wstring> keep;
        FILE *f = nullptr;
        if (!_wfopen_s(&f, PlacementPath().c_str(), L"r, ccs=UTF-8") && f)
        {
            wchar_t line[256];
            while (fgetws(line, 256, f))
            {
                int w = 0, h = 0;
                if (swscanf_s(line, L"%d %d", &w, &h) == 2 && !(w == tabletW && h == tabletH))
                    keep.push_back(line);
            }
            fclose(f);
        }
        if (_wfopen_s(&f, PlacementPath().c_str(), L"w, ccs=UTF-8") || !f)
            return;
        for (auto &l : keep)
            fputws(l.c_str(), f);
        fwprintf(f, L"%d %d %d %d %d %d %d\n", tabletW, tabletH, p.x, p.y, p.mode.width, p.mode.height, p.mode.hz);
        fclose(f);
        LOGI("display: remembered virtual monitor for %dx%d tablet: (%d,%d) %dx%d@%d", tabletW, tabletH, p.x, p.y, p.mode.width,
             p.mode.height, p.mode.hz);
    }

    bool LoadVirtualPlacement(int tabletW, int tabletH, VirtualPlacement &out)
    {
        FILE *f = nullptr;
        if (_wfopen_s(&f, PlacementPath().c_str(), L"r, ccs=UTF-8") || !f)
            return false;
        bool found = false;
        int w, h;
        VirtualPlacement p;
        while (fwscanf_s(f, L"%d %d %d %d %d %d %d", &w, &h, &p.x, &p.y, &p.mode.width, &p.mode.height, &p.mode.hz) == 7)
            if (w == tabletW && h == tabletH)
                out = p, found = true;
        fclose(f);
        return found;
    }

    static bool HasMode(const std::vector<DisplayMode> &modes, int width, int height, int hz)
    {
        return std::any_of(modes.begin(), modes.end(), [&](auto &m)
                           { return m.width == width && m.height == height && m.hz == hz; });
    }

    // The user's setup (saved when their last session ended) wins over wherever Windows put the monitor: enabling the device makes
    // Windows re-attach it using its own stored topology. The tablet app's refresh choice still applies.
    static DisplayMode SavedModeOr(const std::vector<DisplayMode> &modes, DisplayMode want, bool matchResolution, const VirtualPlacement &saved)
    {
        DisplayMode base = saved.mode;
        if (!HasMode(modes, base.width, base.height, base.hz))
            base = PickMode(modes, want, matchResolution, nullptr);
        if (want.hz > 0 && HasMode(modes, base.width, base.height, want.hz))
            base.hz = want.hz;
        return base;
    }

    static LONG ApplyMode(const std::wstring &gdiName, DisplayMode target, const VirtualPlacement *saved, bool attached)
    {
        DEVMODEW dm{};
        dm.dmSize = sizeof(dm);
        dm.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_DISPLAYFREQUENCY | DM_BITSPERPEL;
        dm.dmPelsWidth = target.width;
        dm.dmPelsHeight = target.height;
        dm.dmDisplayFrequency = target.hz;
        dm.dmBitsPerPel = 32;
        if (saved)
        {
            dm.dmFields |= DM_POSITION;
            dm.dmPosition = {saved->x, saved->y};
        }
        else if (!attached)
        {
            RECT b = DesktopBounds();
            dm.dmFields |= DM_POSITION;
            dm.dmPosition = {b.right, 0};
        }
        LONG r = ChangeDisplaySettingsExW(gdiName.c_str(), &dm, nullptr, CDS_UPDATEREGISTRY | CDS_NORESET, nullptr);
        if (r == DISP_CHANGE_SUCCESSFUL)
            r = ChangeDisplaySettingsExW(nullptr, nullptr, nullptr, 0, nullptr);
        return r;
    }

    bool EnsureAttached(const std::wstring &gdiName, DisplayMode want, bool matchResolution, const VirtualPlacement *saved,
                        DisplayMode &active)
    {
        DisplayMode cur{};
        bool attached = IsAttached(gdiName);
        bool haveCur = attached && CurrentMode(gdiName, cur);
        auto modes = ListModes(gdiName);
        VirtualPlacement now{};
        bool havePos = attached && CurrentPlacement(gdiName, now);
        const DisplayMode target = saved ? SavedModeOr(modes, want, matchResolution, *saved) : PickMode(modes, want, matchResolution, haveCur ? &cur : nullptr);

        bool rightMode = haveCur && cur.width == target.width && cur.height == target.height && cur.hz == target.hz;
        bool rightPlace = !saved || (havePos && now.x == saved->x && now.y == saved->y);
        if (attached && rightMode && rightPlace)
        {
            active = cur;
            return true;
        }

        const LONG r = ApplyMode(gdiName, target, saved, attached);
        LOGI("display: %s %ls -> %dx%d@%d%s (result %ld)", attached ? "mode change" : "attach", gdiName.c_str(), target.width,
             target.height, target.hz, saved ? " (your saved setup)" : "", r);

        if (r != DISP_CHANGE_SUCCESSFUL && !attached)
        {
            // Fall back to asking Windows to extend onto every connected display.
            LONG s = SetDisplayConfig(0, nullptr, 0, nullptr, SDC_APPLY | SDC_TOPOLOGY_EXTEND);
            LOGI("display: SetDisplayConfig(EXTEND) -> %ld", s);
        }
        for (int i = 0; i < 30; ++i)
        { // the mode switch settles asynchronously
            if (CurrentMode(gdiName, active))
                return true;
            Sleep(100);
        }
        return false;
    }

    void Detach(const std::wstring &gdiName)
    {
        DEVMODEW dm{};
        dm.dmSize = sizeof(dm);
        dm.dmFields = DM_POSITION | DM_PELSWIDTH | DM_PELSHEIGHT;
        LONG r = ChangeDisplaySettingsExW(gdiName.c_str(), &dm, nullptr, CDS_UPDATEREGISTRY | CDS_NORESET, nullptr);
        if (r == DISP_CHANGE_SUCCESSFUL)
            r = ChangeDisplaySettingsExW(nullptr, nullptr, nullptr, 0, nullptr);
        LOGI("display: detach %ls -> %ld", gdiName.c_str(), r);
    }

    void LogDisplays()
    {
        DISPLAY_DEVICEW dd{sizeof(dd)};
        for (DWORD i = 0; EnumDisplayDevicesW(nullptr, i, &dd, 0); ++i, dd = {sizeof(dd)})
        {
            DisplayMode m{};
            bool cur = CurrentMode(dd.DeviceName, m);
            LOGI("source %ls  [%ls]  id=%ls  attached=%d monitor=%d%s", dd.DeviceName, dd.DeviceString, dd.DeviceID,
                 (dd.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP) ? 1 : 0, HasMonitor(dd.DeviceName) ? 1 : 0,
                 IsVirtualAdapter(dd) ? "  <- virtual" : "");
            if (cur)
                LOGI("    current %dx%d@%d", m.width, m.height, m.hz);
        }
        ComPtr<IDXGIFactory1> factory;
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
            return;
        ComPtr<IDXGIAdapter1> adapter;
        for (UINT a = 0; factory->EnumAdapters1(a, &adapter) != DXGI_ERROR_NOT_FOUND; ++a)
        {
            DXGI_ADAPTER_DESC1 ad;
            adapter->GetDesc1(&ad);
            LOGI("adapter %u: %ls (vendor %04x)", a, ad.Description, ad.VendorId);
            ComPtr<IDXGIOutput> out;
            for (UINT o = 0; adapter->EnumOutputs(o, &out) != DXGI_ERROR_NOT_FOUND; ++o)
            {
                DXGI_OUTPUT_DESC od;
                out->GetDesc(&od);
                auto &r = od.DesktopCoordinates;
                LOGI("    output %ls  (%ld,%ld)-(%ld,%ld)", od.DeviceName, r.left, r.top, r.right, r.bottom);
            }
        }
    }

} // namespace pd
