// PadDisplay by Nezucloud. Copyright (c) 2026 Nezucloud.
// SPDX-License-Identifier: MIT
#include "display.h"
#include "log.h"
#include "settings.h"

#include <dxgi1_6.h>
#include <wrl/client.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cwctype>
#include <mutex>
#include <thread>

using Microsoft::WRL::ComPtr;

namespace pd {

static std::wstring Lower(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return wchar_t(std::towlower(c)); });
    return s;
}

static bool IsVirtualAdapter(const DISPLAY_DEVICEW& dd) {
    std::wstring desc = Lower(dd.DeviceString), id = Lower(dd.DeviceID);
    return desc.find(L"virtual display") != std::wstring::npos || id.find(L"mttvdd") != std::wstring::npos ||
           id.find(L"root\\display") != std::wstring::npos || id.find(L"iddsampledriver") != std::wstring::npos;
}

static bool HasMonitor(const std::wstring& gdiName) {
    DISPLAY_DEVICEW mon{sizeof(mon)};
    return EnumDisplayDevicesW(gdiName.c_str(), 0, &mon, 0) != FALSE;
}

bool FindVirtualDisplay(const std::wstring& overrideName, VirtualDisplay& out) {
    VirtualDisplay best;
    bool found = false;
    DISPLAY_DEVICEW dd{sizeof(dd)};
    for (DWORD i = 0; EnumDisplayDevicesW(nullptr, i, &dd, 0); ++i, dd = {sizeof(dd)}) {
        bool match = overrideName.empty() ? IsVirtualAdapter(dd) : _wcsicmp(dd.DeviceName, overrideName.c_str()) == 0;
        if (!match) continue;
        bool attached = (dd.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP) != 0;
        if (!attached && !HasMonitor(dd.DeviceName)) continue;
        // Prefer a source that is already attached to the desktop.
        if (!found || (attached && !best.attached)) {
            best = {dd.DeviceName, dd.DeviceString, attached};
            found = true;
        }
    }
    if (found) out = best;
    return found;
}

std::vector<DisplayMode> ListModes(const std::wstring& gdiName) {
    std::vector<DisplayMode> modes;
    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    for (DWORD i = 0; EnumDisplaySettingsExW(gdiName.c_str(), i, &dm, 0); ++i) {
        if (dm.dmBitsPerPel != 32) continue;
        DisplayMode m{int(dm.dmPelsWidth), int(dm.dmPelsHeight), int(dm.dmDisplayFrequency)};
        if (std::none_of(modes.begin(), modes.end(), [&](auto& x) { return x.width == m.width && x.height == m.height && x.hz == m.hz; }))
            modes.push_back(m);
    }
    return modes;
}

static bool CurrentMode(const std::wstring& gdiName, DisplayMode& m) {
    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    if (!EnumDisplaySettingsExW(gdiName.c_str(), ENUM_CURRENT_SETTINGS, &dm, 0) || dm.dmPelsWidth == 0) return false;
    m = {int(dm.dmPelsWidth), int(dm.dmPelsHeight), int(dm.dmDisplayFrequency)};
    return true;
}

// Rank refresh rates: exact match first, then the highest below, then the lowest above.
static int HzScore(int hz, int want) {
    if (hz == want) return 1000000;
    if (hz < want) return 500000 + hz;
    return 250000 - hz;
}

static DisplayMode PickMode(const std::vector<DisplayMode>& modes, DisplayMode want, bool matchResolution, const DisplayMode* current) {
    const DisplayMode* best = nullptr;
    auto consider = [&](auto pred) {
        for (auto& m : modes)
            if (pred(m) && (!best || HzScore(m.hz, want.hz) > HzScore(best->hz, want.hz))) best = &m;
    };
    if (matchResolution) consider([&](auto& m) { return m.width == want.width && m.height == want.height; });
    if (!best && current) consider([&](auto& m) { return m.width == current->width && m.height == current->height; });
    if (!best) {
        // Largest mode that fits the client screen, otherwise 1920x1080-ish.
        long long bestArea = -1;
        for (auto& m : modes) {
            long long area = 1LL * m.width * m.height;
            bool fits = m.width <= std::max(want.width, 1920) && m.height <= std::max(want.height, 1080);
            if (fits && (area > bestArea || (area == bestArea && HzScore(m.hz, want.hz) > HzScore(best->hz, want.hz)))) {
                best = &m;
                bestArea = area;
            }
        }
    }
    return best ? *best : (current ? *current : want);
}

static RECT DesktopBounds() {
    RECT u{0, 0, 0, 0};
    EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR, HDC, LPRECT r, LPARAM p) -> BOOL {
        UnionRect(reinterpret_cast<RECT*>(p), reinterpret_cast<RECT*>(p), r);
        return TRUE;
    }, reinterpret_cast<LPARAM>(&u));
    return u;
}

static bool IsAttached(const std::wstring& gdiName) {
    DISPLAY_DEVICEW dd{sizeof(dd)};
    for (DWORD i = 0; EnumDisplayDevicesW(nullptr, i, &dd, 0); ++i, dd = {sizeof(dd)})
        if (_wcsicmp(dd.DeviceName, gdiName.c_str()) == 0) return (dd.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP) != 0;
    return false;
}

bool CurrentPlacement(const std::wstring& gdiName, VirtualPlacement& out) {
    if (!IsAttached(gdiName)) return false;
    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    if (!EnumDisplaySettingsExW(gdiName.c_str(), ENUM_CURRENT_SETTINGS, &dm, 0) || dm.dmPelsWidth == 0) return false;
    out = {dm.dmPosition.x, dm.dmPosition.y, {int(dm.dmPelsWidth), int(dm.dmPelsHeight), int(dm.dmDisplayFrequency)}};
    return true;
}

static std::wstring PlacementPath() { return AppDataDir() + L"\\virtual-monitor.txt"; }

void SaveVirtualPlacement(int tabletW, int tabletH, const VirtualPlacement& p) {
    // One line per tablet: "tabletW tabletH x y width height hz"; replace this tablet's line.
    std::vector<std::wstring> keep;
    FILE* f = nullptr;
    if (!_wfopen_s(&f, PlacementPath().c_str(), L"r, ccs=UTF-8") && f) {
        wchar_t line[256];
        while (fgetws(line, 256, f)) {
            int w = 0, h = 0;
            if (swscanf_s(line, L"%d %d", &w, &h) == 2 && !(w == tabletW && h == tabletH)) keep.push_back(line);
        }
        fclose(f);
    }
    if (_wfopen_s(&f, PlacementPath().c_str(), L"w, ccs=UTF-8") || !f) return;
    for (auto& l : keep) fputws(l.c_str(), f);
    fwprintf(f, L"%d %d %d %d %d %d %d\n", tabletW, tabletH, p.x, p.y, p.mode.width, p.mode.height, p.mode.hz);
    fclose(f);
    LOGI("display: remembered virtual monitor for %dx%d tablet: (%d,%d) %dx%d@%d", tabletW, tabletH, p.x, p.y, p.mode.width,
         p.mode.height, p.mode.hz);
}

bool LoadVirtualPlacement(int tabletW, int tabletH, VirtualPlacement& out) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, PlacementPath().c_str(), L"r, ccs=UTF-8") || !f) return false;
    bool found = false;
    int w, h;
    VirtualPlacement p;
    while (fwscanf_s(f, L"%d %d %d %d %d %d %d", &w, &h, &p.x, &p.y, &p.mode.width, &p.mode.height, &p.mode.hz) == 7)
        if (w == tabletW && h == tabletH) out = p, found = true;
    fclose(f);
    return found;
}

bool EnsureAttached(const std::wstring& gdiName, DisplayMode want, bool matchResolution, const VirtualPlacement* saved,
                    DisplayMode& active) {
    DisplayMode cur{};
    bool attached = IsAttached(gdiName);
    bool haveCur = attached && CurrentMode(gdiName, cur);
    auto modes = ListModes(gdiName);
    auto hasMode = [&](int w, int h, int hz) {
        return std::any_of(modes.begin(), modes.end(), [&](auto& m) { return m.width == w && m.height == h && m.hz == hz; });
    };

    DisplayMode target;
    VirtualPlacement now{};
    bool havePos = attached && CurrentPlacement(gdiName, now);
    if (saved) {
        // The user's setup (saved when their last session ended) wins over wherever Windows put the
        // monitor: enabling the device makes Windows re-attach it using its own stored topology.
        // The tablet app's refresh choice still applies.
        DisplayMode base = saved->mode;
        if (!hasMode(base.width, base.height, base.hz)) base = PickMode(modes, want, matchResolution, nullptr);
        target = base;
        if (want.hz > 0 && hasMode(base.width, base.height, want.hz)) target.hz = want.hz;
    } else {
        target = PickMode(modes, want, matchResolution, haveCur ? &cur : nullptr);
    }

    bool rightMode = haveCur && cur.width == target.width && cur.height == target.height && cur.hz == target.hz;
    bool rightPlace = !saved || (havePos && now.x == saved->x && now.y == saved->y);
    if (attached && rightMode && rightPlace) {
        active = cur;
        return true;
    }

    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    dm.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_DISPLAYFREQUENCY | DM_BITSPERPEL;
    dm.dmPelsWidth = target.width;
    dm.dmPelsHeight = target.height;
    dm.dmDisplayFrequency = target.hz;
    dm.dmBitsPerPel = 32;
    if (saved) {
        dm.dmFields |= DM_POSITION;
        dm.dmPosition = {saved->x, saved->y};
    } else if (!attached) {
        RECT b = DesktopBounds();
        dm.dmFields |= DM_POSITION;
        dm.dmPosition = {b.right, 0};
    }
    LONG r = ChangeDisplaySettingsExW(gdiName.c_str(), &dm, nullptr, CDS_UPDATEREGISTRY | CDS_NORESET, nullptr);
    if (r == DISP_CHANGE_SUCCESSFUL) r = ChangeDisplaySettingsExW(nullptr, nullptr, nullptr, 0, nullptr);
    LOGI("display: %s %ls -> %dx%d@%d%s (result %ld)", attached ? "mode change" : "attach", gdiName.c_str(), target.width,
         target.height, target.hz, saved ? " (your saved setup)" : "", r);

    if (r != DISP_CHANGE_SUCCESSFUL && !attached) {
        // Fall back to asking Windows to extend onto every connected display.
        LONG s = SetDisplayConfig(0, nullptr, 0, nullptr, SDC_APPLY | SDC_TOPOLOGY_EXTEND);
        LOGI("display: SetDisplayConfig(EXTEND) -> %ld", s);
    }
    for (int i = 0; i < 30; ++i) { // the mode switch settles asynchronously
        if (CurrentMode(gdiName, active)) return true;
        Sleep(100);
    }
    return false;
}

void Detach(const std::wstring& gdiName) {
    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    dm.dmFields = DM_POSITION | DM_PELSWIDTH | DM_PELSHEIGHT;
    LONG r = ChangeDisplaySettingsExW(gdiName.c_str(), &dm, nullptr, CDS_UPDATEREGISTRY | CDS_NORESET, nullptr);
    if (r == DISP_CHANGE_SUCCESSFUL) r = ChangeDisplaySettingsExW(nullptr, nullptr, nullptr, 0, nullptr);
    LOGI("display: detach %ls -> %ld", gdiName.c_str(), r);
}

std::vector<DisplayPlacement> SnapshotRealDisplays() {
    std::vector<DisplayPlacement> out;
    DISPLAY_DEVICEW dd{sizeof(dd)};
    for (DWORD i = 0; EnumDisplayDevicesW(nullptr, i, &dd, 0); ++i, dd = {sizeof(dd)}) {
        if (!(dd.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP) || IsVirtualAdapter(dd)) continue;
        DEVMODEW dm{};
        dm.dmSize = sizeof(dm);
        if (!EnumDisplaySettingsExW(dd.DeviceName, ENUM_CURRENT_SETTINGS, &dm, 0)) continue;
        out.push_back({dd.DeviceName, dm.dmPosition.x, dm.dmPosition.y, int(dm.dmPelsWidth), int(dm.dmPelsHeight),
                       int(dm.dmDisplayFrequency)});
    }
    return out;
}

static std::wstring HomeLayoutPath() { return AppDataDir() + L"\\home-layout.txt"; }

static void SaveHomeLayout(const std::vector<DisplayPlacement>& layout) {
    if (layout.empty()) return;
    FILE* f = nullptr;
    if (_wfopen_s(&f, HomeLayoutPath().c_str(), L"w, ccs=UTF-8") || !f) return;
    for (auto& p : layout) fwprintf(f, L"%ls %d %d %d %d %d\n", p.gdiName.c_str(), p.x, p.y, p.width, p.height, p.hz);
    fclose(f);
}

std::vector<DisplayPlacement> LoadHomeLayout() {
    std::vector<DisplayPlacement> out;
    FILE* f = nullptr;
    if (_wfopen_s(&f, HomeLayoutPath().c_str(), L"r, ccs=UTF-8") || !f) return out;
    wchar_t name[64];
    DisplayPlacement p;
    while (fwscanf_s(f, L"%63ls %d %d %d %d %d", name, unsigned(_countof(name)), &p.x, &p.y, &p.width, &p.height, &p.hz) == 6) {
        p.gdiName = name;
        out.push_back(p);
    }
    fclose(f);
    return out;
}

namespace {

// Screens that are on, in the form SetDisplayConfig takes: a path per screen, and the modes the paths point to.
struct ScreenSetup {
    std::vector<DISPLAYCONFIG_PATH_INFO> paths;
    std::vector<DISPLAYCONFIG_MODE_INFO> modes;
};

std::mutex g_rememberedMutex;
ScreenSetup g_remembered; // see RememberRealScreens; it names the graphics adapters by their IDs, which hold until Windows restarts

bool QueryActiveScreens(ScreenSetup& all) {
    for (int attempt = 0; attempt < 3; ++attempt) { // a monitor arriving between the two calls makes the buffers too small
        UINT32 pathCount = 0, modeCount = 0;
        if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount) != ERROR_SUCCESS) return false;
        all.paths.resize(pathCount);
        all.modes.resize(modeCount);
        LONG r = QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, all.paths.data(), &modeCount, all.modes.data(), nullptr);
        if (r == ERROR_SUCCESS) {
            all.paths.resize(pathCount);
            all.modes.resize(modeCount);
            return true;
        }
        if (r != ERROR_INSUFFICIENT_BUFFER) return false;
    }
    return false;
}

std::wstring SourceName(const DISPLAYCONFIG_PATH_INFO& path) {
    DISPLAYCONFIG_SOURCE_DEVICE_NAME name{};
    name.header = {DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME, sizeof(name), path.sourceInfo.adapterId, path.sourceInfo.id};
    return DisplayConfigGetDeviceInfo(&name.header) == ERROR_SUCCESS ? name.viewGdiDeviceName : L"";
}

bool IsVirtualSource(const std::wstring& gdiName) {
    DISPLAY_DEVICEW dd{sizeof(dd)};
    for (DWORD i = 0; EnumDisplayDevicesW(nullptr, i, &dd, 0); ++i, dd = {sizeof(dd)})
        if (_wcsicmp(dd.DeviceName, gdiName.c_str()) == 0) return IsVirtualAdapter(dd);
    return false;
}

bool SameAdapter(const LUID& a, const LUID& b) { return a.LowPart == b.LowPart && a.HighPart == b.HighPart; }

// Copies a path, and the modes it points to, from one setup to the end of another.
void AppendPath(const ScreenSetup& from, const DISPLAYCONFIG_PATH_INFO& path, ScreenSetup& to) {
    auto copyMode = [&](UINT32 index) -> UINT32 {
        if (index >= from.modes.size()) return DISPLAYCONFIG_PATH_MODE_IDX_INVALID;
        const DISPLAYCONFIG_MODE_INFO& mode = from.modes[index];
        for (UINT32 i = 0; i < to.modes.size(); ++i) // duplicated screens share one source mode
            if (to.modes[i].infoType == mode.infoType && to.modes[i].id == mode.id && SameAdapter(to.modes[i].adapterId, mode.adapterId)) return i;
        to.modes.push_back(mode);
        return UINT32(to.modes.size() - 1);
    };
    DISPLAYCONFIG_PATH_INFO copy = path;
    copy.sourceInfo.modeInfoIdx = copyMode(path.sourceInfo.modeInfoIdx);
    copy.targetInfo.modeInfoIdx = copyMode(path.targetInfo.modeInfoIdx);
    to.paths.push_back(copy);
}

void SplitScreens(const ScreenSetup& all, ScreenSetup& real, ScreenSetup& virtualMonitor) {
    for (auto& path : all.paths) AppendPath(all, path, IsVirtualSource(SourceName(path)) ? virtualMonitor : real);
}

const DISPLAYCONFIG_MODE_INFO* ModeAt(const ScreenSetup& setup, UINT32 index) {
    return index < setup.modes.size() ? &setup.modes[index] : nullptr;
}

bool SamePlaceAndMode(const ScreenSetup& a, const DISPLAYCONFIG_PATH_INFO& pa, const ScreenSetup& b, const DISPLAYCONFIG_PATH_INFO& pb) {
    const DISPLAYCONFIG_MODE_INFO* sourceA = ModeAt(a, pa.sourceInfo.modeInfoIdx);
    const DISPLAYCONFIG_MODE_INFO* sourceB = ModeAt(b, pb.sourceInfo.modeInfoIdx);
    const DISPLAYCONFIG_MODE_INFO* targetA = ModeAt(a, pa.targetInfo.modeInfoIdx);
    const DISPLAYCONFIG_MODE_INFO* targetB = ModeAt(b, pb.targetInfo.modeInfoIdx);
    if (!sourceA || !sourceB || !targetA || !targetB) return false;
    const DISPLAYCONFIG_SOURCE_MODE& ma = sourceA->sourceMode;
    const DISPLAYCONFIG_SOURCE_MODE& mb = sourceB->sourceMode;
    const DISPLAYCONFIG_RATIONAL& ra = targetA->targetMode.targetVideoSignalInfo.vSyncFreq;
    const DISPLAYCONFIG_RATIONAL& rb = targetB->targetMode.targetVideoSignalInfo.vSyncFreq;
    return ma.width == mb.width && ma.height == mb.height && ma.position.x == mb.position.x && ma.position.y == mb.position.y &&
           ra.Numerator == rb.Numerator && ra.Denominator == rb.Denominator && pa.targetInfo.rotation == pb.targetInfo.rotation;
}

bool SameScreens(const ScreenSetup& a, const ScreenSetup& b) {
    if (a.paths.size() != b.paths.size()) return false;
    for (auto& pa : a.paths) {
        auto it = std::find_if(b.paths.begin(), b.paths.end(), [&](auto& pb) {
            return SameAdapter(pa.sourceInfo.adapterId, pb.sourceInfo.adapterId) && pa.sourceInfo.id == pb.sourceInfo.id &&
                   pa.targetInfo.id == pb.targetInfo.id;
        });
        if (it == b.paths.end() || !SamePlaceAndMode(a, pa, b, *it)) return false;
    }
    return true;
}

std::wstring Describe(const ScreenSetup& setup) {
    std::wstring text;
    for (auto& path : setup.paths) {
        const DISPLAYCONFIG_MODE_INFO* source = ModeAt(setup, path.sourceInfo.modeInfoIdx);
        const DISPLAYCONFIG_MODE_INFO* target = ModeAt(setup, path.targetInfo.modeInfoIdx);
        if (!source || !target) continue;
        const DISPLAYCONFIG_RATIONAL& rate = target->targetMode.targetVideoSignalInfo.vSyncFreq;
        wchar_t one[160];
        swprintf_s(one, L"%ls (%ld,%ld) %ux%u@%u", SourceName(path).c_str(), source->sourceMode.position.x, source->sourceMode.position.y,
                   source->sourceMode.width, source->sourceMode.height,
                   rate.Denominator ? (rate.Numerator + rate.Denominator / 2) / rate.Denominator : 0);
        text += (text.empty() ? L"" : L", ") + std::wstring(one);
    }
    return text.empty() ? L"none" : text;
}

enum class PutBack { AlreadyThere, Done, Refused, NothingRemembered };

// The remembered real screens in one SetDisplayConfig; with keepVirtual, the virtual monitor stays where it is now.
PutBack PutRealScreensBack(bool keepVirtual) {
    ScreenSetup want;
    {
        std::lock_guard lock(g_rememberedMutex);
        want = g_remembered;
    }
    if (want.paths.empty()) return PutBack::NothingRemembered;
    ScreenSetup all, real, virtualMonitor;
    if (!QueryActiveScreens(all)) return PutBack::Refused;
    SplitScreens(all, real, virtualMonitor);
    if (SameScreens(real, want) && (keepVirtual || virtualMonitor.paths.empty())) return PutBack::AlreadyThere;
    if (keepVirtual)
        for (auto& path : virtualMonitor.paths) AppendPath(virtualMonitor, path, want);
    // Saved as Windows' setup for the monitors that are connected now, unlike only-screen mode. A monitor that was dark for
    // a while can drop off the bus when it wakes (measured: gone for 1.1 s, half a second after it was switched on). Windows
    // then applies what it has stored, once without that monitor and once with it, and that must be this setup, not an older one.
    const ULONGLONG start = GetTickCount64();
    LONG r = SetDisplayConfig(UINT32(want.paths.size()), want.paths.data(), UINT32(want.modes.size()), want.modes.data(),
                              SDC_APPLY | SDC_USE_SUPPLIED_DISPLAY_CONFIG | SDC_ALLOW_CHANGES | SDC_SAVE_TO_DATABASE);
    if (r != ERROR_SUCCESS) {
        LOGI("display: the PC's screens are %ls; Windows refused to put them back as you had them (%ld)", Describe(real).c_str(), r);
        return PutBack::Refused;
    }
    LOGI("display: the PC's screens were %ls; put back as you had them%s in %llu ms", Describe(real).c_str(),
         keepVirtual ? "" : ", virtual monitor off the desktop", GetTickCount64() - start);
    return PutBack::Done;
}

// A monitor that is plugged in but not part of the desktop. QDC_ALL_PATHS lists every output with every source that could drive it.
bool ARealScreenIsOff() {
    UINT32 pathCount = 0, modeCount = 0;
    if (GetDisplayConfigBufferSizes(QDC_ALL_PATHS, &pathCount, &modeCount) != ERROR_SUCCESS) return false;
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
    if (QueryDisplayConfig(QDC_ALL_PATHS, &pathCount, paths.data(), &modeCount, modes.data(), nullptr) != ERROR_SUCCESS) return false;
    paths.resize(pathCount);
    auto isOn = [&](const DISPLAYCONFIG_PATH_INFO& output) {
        return std::any_of(paths.begin(), paths.end(), [&](auto& p) {
            return (p.flags & DISPLAYCONFIG_PATH_ACTIVE) && p.targetInfo.id == output.targetInfo.id &&
                   SameAdapter(p.targetInfo.adapterId, output.targetInfo.adapterId);
        });
    };
    return std::any_of(paths.begin(), paths.end(),
                       [&](auto& p) { return p.targetInfo.targetAvailable && !isOn(p) && !IsVirtualSource(SourceName(p)); });
}

DEVMODEW ToDevMode(const DisplayPlacement& p) {
    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    dm.dmFields = DM_POSITION | DM_PELSWIDTH | DM_PELSHEIGHT | DM_DISPLAYFREQUENCY | DM_BITSPERPEL;
    dm.dmPosition = {p.x, p.y};
    dm.dmPelsWidth = p.width;
    dm.dmPelsHeight = p.height;
    dm.dmDisplayFrequency = p.hz;
    dm.dmBitsPerPel = 32;
    return dm;
}

// The file's layout, for a run that has not remembered the screens itself (see display.h). Returns 1 if it changed them.
// All screens go in one batch, the main display first, and only if Windows accepts every one of them: moving just the
// screens that differ can leave two at the same place, and Windows then arranges them its own way.
int RestoreFromFile(bool onlyIfOneIsOff) {
    auto saved = LoadHomeLayout();
    auto now = SnapshotRealDisplays();
    bool differs = false;
    for (auto& s : saved) {
        auto it = std::find_if(now.begin(), now.end(), [&](auto& n) { return _wcsicmp(n.gdiName.c_str(), s.gdiName.c_str()) == 0; });
        differs |= it == now.end() || it->x != s.x || it->y != s.y || it->width != s.width || it->height != s.height || abs(it->hz - s.hz) > 1;
    }
    if (!differs || (onlyIfOneIsOff && !ARealScreenIsOff())) return 0;
    std::stable_sort(saved.begin(), saved.end(), [](auto& a, auto& b) { return (a.x == 0 && a.y == 0) && !(b.x == 0 && b.y == 0); });
    for (auto& s : saved) {
        DEVMODEW dm = ToDevMode(s);
        if (ChangeDisplaySettingsExW(s.gdiName.c_str(), &dm, nullptr, CDS_TEST, nullptr) != DISP_CHANGE_SUCCESSFUL) return 0; // not now: Windows is still settling, or the file is out of date
    }
    LONG r = DISP_CHANGE_SUCCESSFUL;
    for (auto& s : saved) {
        DEVMODEW dm = ToDevMode(s);
        DWORD flags = CDS_UPDATEREGISTRY | CDS_NORESET | (s.x == 0 && s.y == 0 ? CDS_SET_PRIMARY : 0);
        LONG one = ChangeDisplaySettingsExW(s.gdiName.c_str(), &dm, nullptr, flags, nullptr);
        LOGI("display: %ls back at (%d,%d) %dx%d@%d, from home-layout.txt -> %ld", s.gdiName.c_str(), s.x, s.y, s.width, s.height, s.hz, one);
        if (one != DISP_CHANGE_SUCCESSFUL) r = one;
    }
    ChangeDisplaySettingsExW(nullptr, nullptr, nullptr, 0, nullptr);
    return r == DISP_CHANGE_SUCCESSFUL ? 1 : 0;
}

} // namespace

void RememberRealScreens() {
    ScreenSetup all, real, virtualMonitor;
    if (!QueryActiveScreens(all)) return;
    SplitScreens(all, real, virtualMonitor);
    if (real.paths.empty()) return; // a tablet is the only screen, or nothing is on: no arrangement to come back to
    LOGI("display: remembered the PC's screens: %ls", Describe(real).c_str());
    SaveHomeLayout(SnapshotRealDisplays());
    std::lock_guard lock(g_rememberedMutex);
    g_remembered = std::move(real);
}

int RestoreRealScreens() {
    int fixed = 0;
    // 10 x 150 ms: Windows applies its own stored layout a moment after a monitor came or went, and has no event that says
    // it is done. At most 3 corrections: if Windows keeps adjusting what it is given, more would only make the screens flicker.
    for (int i = 0; i < 10 && fixed < 3; ++i) {
        Sleep(150);
        switch (PutRealScreensBack(true)) {
        case PutBack::Done: ++fixed; break;
        case PutBack::NothingRemembered: fixed += RestoreFromFile(true); break;
        case PutBack::Refused: fixed += RestoreFromFile(false); break; // e.g. the graphics driver restarted: its IDs changed
        case PutBack::AlreadyThere: break;
        }
    }
    return fixed;
}

bool ShowRealScreensOnly() {
    PutBack r = PutRealScreensBack(false);
    return (r == PutBack::Done || r == PutBack::AlreadyThere) && !SnapshotRealDisplays().empty();
}

bool ShowOnlyOn(const std::wstring& gdiName) {
    UINT32 pathCount = 0, modeCount = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount) != ERROR_SUCCESS) return false;
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(), &modeCount, modes.data(), nullptr) != ERROR_SUCCESS)
        return false;
    paths.resize(pathCount);
    for (auto& path : paths) {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME name{};
        name.header = {DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME, sizeof(name), path.sourceInfo.adapterId, path.sourceInfo.id};
        if (DisplayConfigGetDeviceInfo(&name.header) != ERROR_SUCCESS || _wcsicmp(name.viewGdiDeviceName, gdiName.c_str()) != 0) continue;
        UINT32 source = path.sourceInfo.modeInfoIdx, target = path.targetInfo.modeInfoIdx;
        if (source >= modeCount || target >= modeCount) return false;
        // Only this path, as the primary display. No SDC_SAVE_TO_DATABASE: see display.h.
        DISPLAYCONFIG_MODE_INFO only[2] = {modes[source], modes[target]};
        only[0].sourceMode.position = {0, 0};
        DISPLAYCONFIG_PATH_INFO p = path;
        p.sourceInfo.modeInfoIdx = 0;
        p.targetInfo.modeInfoIdx = 1;
        LONG r = SetDisplayConfig(1, &p, 2, only, SDC_APPLY | SDC_USE_SUPPLIED_DISPLAY_CONFIG | SDC_ALLOW_CHANGES);
        LOGI("display: %ls is now the only screen (%u other screen%s off) -> %ld", gdiName.c_str(), pathCount - 1,
             pathCount == 2 ? "" : "s", r);
        return r == ERROR_SUCCESS;
    }
    return false;
}

namespace {
std::atomic<bool> g_restoring{false};
std::atomic<bool> g_cancelRestore{false};

// While a security prompt (UAC) is up, Windows runs it on its own desktop and refuses every display
// change from other programs (ERROR_ACCESS_DENIED) until it is answered. One failed attempt must
// not leave the PC's own screens off for good, so keep trying, in the background, until they are on.
void RestoreInBackground() {
    if (g_restoring.exchange(true)) return;
    g_cancelRestore = false;
    std::thread([] {
        const ULONGLONG giveUp = GetTickCount64() + 10 * 60 * 1000;
        bool back = false;
        LONG r = 0;
        // 500 ms: a poll, because Windows has no event for "the prompt was answered"; it only runs while the screens are off.
        while (!g_cancelRestore && GetTickCount64() < giveUp) {
            r = SetDisplayConfig(0, nullptr, 0, nullptr, SDC_APPLY | SDC_USE_DATABASE_CURRENT);
            if (r == ERROR_SUCCESS && !SnapshotRealDisplays().empty()) {
                back = true;
                break;
            }
            Sleep(500);
        }
        if (back) {
            LOGI("display: the PC's own screens are back");
            RestoreRealScreens();
        } else {
            LOGI("display: gave up bringing the PC's own screens back (last result %ld)", r);
        }
        g_restoring = false;
    }).detach();
}
} // namespace

bool ShowOnAllAgain() {
    LONG r = SetDisplayConfig(0, nullptr, 0, nullptr, SDC_APPLY | SDC_USE_DATABASE_CURRENT);
    bool back = false;
    for (int i = 0; i < 10 && !back; ++i) {
        if (i) Sleep(100);
        back = !SnapshotRealDisplays().empty();
    }
    LOGI("display: back to the stored setup -> %ld, real screens %s", r, back ? "on" : "STILL OFF");
    if (!back) RestoreInBackground();
    return back;
}

void CancelScreenRestore() { g_cancelRestore = true; }

void WaitScreenRestore(unsigned timeoutMs) {
    for (ULONGLONG end = GetTickCount64() + timeoutMs; g_restoring && GetTickCount64() < end;) Sleep(50);
}

// --- UAC prompts the tablet can click, in only-screen mode -----------------------------------
// A UAC prompt normally appears on the secure desktop, which Desktop Duplication cannot capture. In
// only-screen mode the PC's real screens are off, so the prompt is invisible and unanswerable.
// Setting PromptOnSecureDesktop = 0 moves prompts to the normal desktop: the tablet then shows them,
// and - because an elevated host injects input at the same (high) integrity as the consent dialog -
// can click them. The value is machine-wide, so it is changed only while a tablet is the only screen
// and restored the moment that ends. The old value goes to a marker file first, so a crash is
// recoverable (RestoreSecureDesktop runs at start-up too).
namespace {
constexpr wchar_t kUacPolicyKey[] = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Policies\\System";
constexpr wchar_t kUacPolicyValue[] = L"PromptOnSecureDesktop";

std::wstring SecureDesktopMarker() { return AppDataDir() + L"\\secure-desktop.txt"; }

bool IsProcessElevated() {
    HANDLE token = nullptr;
    TOKEN_ELEVATION e{};
    DWORD n = 0;
    bool elevated = OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token) &&
                    GetTokenInformation(token, TokenElevation, &e, sizeof(e), &n) && e.TokenIsElevated;
    if (token) CloseHandle(token);
    return elevated;
}

bool ReadUacPolicy(DWORD& value) {
    DWORD size = sizeof(value);
    LONG r = RegGetValueW(HKEY_LOCAL_MACHINE, kUacPolicyKey, kUacPolicyValue, RRF_RT_REG_DWORD, nullptr, &value, &size);
    if (r == ERROR_FILE_NOT_FOUND) { value = 1; return true; } // absent on a default install = secure desktop on
    return r == ERROR_SUCCESS;
}

bool WriteUacPolicy(DWORD value) {
    return RegSetKeyValueW(HKEY_LOCAL_MACHINE, kUacPolicyKey, kUacPolicyValue, REG_DWORD, &value, sizeof(value)) == ERROR_SUCCESS;
}
} // namespace

bool HostIsElevated() { return IsProcessElevated(); }

// Set from the tray message thread, read from the session thread: a plain atomic is enough (no
// ordering with other state), and it never blocks a hot thread.
namespace { std::atomic<bool> g_sessionLocked{false}; }
void SetSessionLocked(bool locked) { g_sessionLocked.store(locked, std::memory_order_relaxed); }
bool SessionIsLocked() { return g_sessionLocked.load(std::memory_order_relaxed); }

bool AllowUacClicksOnTablet() {
    if (!IsProcessElevated()) { // writing HKLM policy, and clicking the elevated prompt, both need elevation
        LOGI("uac: not moving prompts off the secure desktop - PadDisplay is not elevated");
        return false;
    }
    DWORD current = 1;
    if (!ReadUacPolicy(current)) {
        LOGI("uac: could not read PromptOnSecureDesktop");
        return false;
    }
    if (current == 0) return true; // already on the normal desktop: nothing to change, nothing to undo
    // Record the old value before touching the policy, so a crash cannot strand the secure desktop off.
    if (FILE* f = nullptr; !_wfopen_s(&f, SecureDesktopMarker().c_str(), L"w") && f) {
        fwprintf(f, L"%lu", current);
        fclose(f);
    }
    if (!WriteUacPolicy(0)) {
        LOGI("uac: could not move prompts off the secure desktop");
        DeleteFileW(SecureDesktopMarker().c_str());
        return false;
    }
    LOGI("uac: prompts moved to the normal desktop so the tablet can show and click them (was %lu)", current);
    return true;
}

void RestoreSecureDesktop() {
    const std::wstring marker = SecureDesktopMarker();
    FILE* f = nullptr;
    if (_wfopen_s(&f, marker.c_str(), L"r") || !f) return; // nothing was changed
    unsigned long saved = 1;
    if (fwscanf_s(f, L"%lu", &saved) != 1) saved = 1;
    fclose(f);
    if (!IsProcessElevated()) { // keep the marker: a later elevated run will put the policy back
        LOGI("uac: secure desktop should be restored to %lu but PadDisplay is not elevated; will retry when elevated", saved);
        return;
    }
    if (WriteUacPolicy(DWORD(saved))) {
        LOGI("uac: secure desktop restored (PromptOnSecureDesktop = %lu)", saved);
        DeleteFileW(marker.c_str());
    } else {
        LOGI("uac: could not restore PromptOnSecureDesktop to %lu (marker kept)", saved);
    }
}

void LogDisplays() {
    DISPLAY_DEVICEW dd{sizeof(dd)};
    for (DWORD i = 0; EnumDisplayDevicesW(nullptr, i, &dd, 0); ++i, dd = {sizeof(dd)}) {
        DisplayMode m{};
        bool cur = CurrentMode(dd.DeviceName, m);
        LOGI("source %ls  [%ls]  id=%ls  attached=%d monitor=%d%s", dd.DeviceName, dd.DeviceString, dd.DeviceID,
             (dd.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP) ? 1 : 0, HasMonitor(dd.DeviceName) ? 1 : 0,
             IsVirtualAdapter(dd) ? "  <- virtual" : "");
        if (cur) LOGI("    current %dx%d@%d", m.width, m.height, m.hz);
    }
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return;
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT a = 0; factory->EnumAdapters1(a, &adapter) != DXGI_ERROR_NOT_FOUND; ++a) {
        DXGI_ADAPTER_DESC1 ad;
        adapter->GetDesc1(&ad);
        LOGI("adapter %u: %ls (vendor %04x)", a, ad.Description, ad.VendorId);
        ComPtr<IDXGIOutput> out;
        for (UINT o = 0; adapter->EnumOutputs(o, &out) != DXGI_ERROR_NOT_FOUND; ++o) {
            DXGI_OUTPUT_DESC od;
            out->GetDesc(&od);
            auto& r = od.DesktopCoordinates;
            LOGI("    output %ls  (%ld,%ld)-(%ld,%ld)", od.DeviceName, r.left, r.top, r.right, r.bottom);
        }
    }
}

} // namespace pd
