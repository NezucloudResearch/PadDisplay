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
#include <cwctype>
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

int RestoreRealDisplays(const std::vector<DisplayPlacement>& saved) {
    auto now = SnapshotRealDisplays();
    int fixed = 0;
    for (auto& s : saved) {
        auto it = std::find_if(now.begin(), now.end(), [&](auto& n) { return _wcsicmp(n.gdiName.c_str(), s.gdiName.c_str()) == 0; });
        if (it != now.end() && it->x == s.x && it->y == s.y && it->width == s.width && it->height == s.height) continue;
        DEVMODEW dm{};
        dm.dmSize = sizeof(dm);
        dm.dmFields = DM_POSITION | DM_PELSWIDTH | DM_PELSHEIGHT | DM_DISPLAYFREQUENCY | DM_BITSPERPEL;
        dm.dmPosition = {s.x, s.y};
        dm.dmPelsWidth = s.width;
        dm.dmPelsHeight = s.height;
        dm.dmDisplayFrequency = s.hz;
        dm.dmBitsPerPel = 32;
        DWORD flags = CDS_UPDATEREGISTRY | CDS_NORESET | (s.x == 0 && s.y == 0 ? CDS_SET_PRIMARY : 0);
        LONG r = ChangeDisplaySettingsExW(s.gdiName.c_str(), &dm, nullptr, flags, nullptr);
        LOGI("display: restoring %ls at (%d,%d) %dx%d@%d (%s) -> %ld", s.gdiName.c_str(), s.x, s.y, s.width, s.height, s.hz,
             it == now.end() ? "was switched off" : "was moved", r);
        if (r == DISP_CHANGE_SUCCESSFUL) ++fixed;
    }
    if (fixed) ChangeDisplaySettingsExW(nullptr, nullptr, nullptr, 0, nullptr);
    return fixed;
}

static std::wstring HomeLayoutPath() { return AppDataDir() + L"\\home-layout.txt"; }

void SaveHomeLayout(const std::vector<DisplayPlacement>& layout) {
    if (layout.empty()) return;
    FILE* f = nullptr;
    if (_wfopen_s(&f, HomeLayoutPath().c_str(), L"w, ccs=UTF-8") || !f) return;
    for (auto& p : layout) fwprintf(f, L"%ls %d %d %d %d %d\n", p.gdiName.c_str(), p.x, p.y, p.width, p.height, p.hz);
    fclose(f);
    LOGI("display: saved home layout (%zu real screens)", layout.size());
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

int RestoreHomeLayout() {
    auto home = LoadHomeLayout();
    if (home.empty()) return 0;
    int fixed = 0;
    for (int i = 0; i < 10; ++i) { // Windows applies its own stored layout a moment after a change
        Sleep(150);
        fixed += RestoreRealDisplays(home);
    }
    return fixed;
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
            RestoreHomeLayout();
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
