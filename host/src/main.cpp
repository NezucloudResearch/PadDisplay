// PadDisplay by Nezucloud. Copyright (c) 2026 Nezucloud.
// SPDX-License-Identifier: MIT
// Host: tray app that streams the virtual monitor to the Android tablet.
//   PadDisplay.exe                 run in the tray
//   PadDisplay.exe --console       same, with a log console
//   PadDisplay.exe --list          print displays/adapters and exit
//   PadDisplay.exe --install-driver
//                                  (elevated) install or enable the bundled virtual monitor driver
//   PadDisplay.exe --dump out.h265 [--seconds 10] [--codec h264|hevc] [--size 2560x1600]
//                                  capture+encode the virtual display to a raw stream file (no tablet needed)
#include "capture.h"
#include "display.h"
#include "log.h"
#include "net.h"
#include "session.h"
#include "settings.h"
#include "gpu_profile.h"
#include "vdd.h"

#include <windows.h>
#include <mfapi.h>
#include <shellapi.h>
#include <memory>
#include <mutex>
#include <string>
#include <atomic>
#include <thread>

using namespace pd;

#define PD_WIDEN2(x) L##x
#define PD_WIDEN(x) PD_WIDEN2(x)
#define PD_VERSION_W PD_WIDEN(PD_VERSION_STR)

namespace {

constexpr UINT WM_TRAY = WM_APP + 1;
constexpr UINT WM_STATUS = WM_APP + 2;
constexpr UINT WM_DRIVER_DONE = WM_APP + 3; // wParam = success

enum MenuIdDriver : UINT { ID_DRIVER = 50 };

enum MenuId : UINT {
    ID_EXIT = 100, ID_OPEN_LOG, ID_OPEN_FOLDER, ID_ADAPTIVE, ID_CURSOR, ID_MATCH_RES, ID_DETACH, ID_AUTOSTART,
    ID_CODEC_AUTO = 200, ID_CODEC_HEVC, ID_CODEC_H264,
    ID_USB_RATE = 300,  // + Mbps
    ID_WIFI_RATE = 400, // + Mbps
};

Settings g_settings;
std::unique_ptr<Server> g_server;
std::mutex g_statusMutex;
std::wstring g_status = L"Waiting for tablet";
HWND g_hwnd = nullptr;
NOTIFYICONDATAW g_nid{};
bool g_installing = false;
bool g_restartAfterInstall = false;
bool g_monitorRemoved = false;
std::atomic<int> g_setupIncomplete{-1}; // -1 unknown (checked on a worker thread), 0 complete, 1 needs the admin run

// On exit / sign-out / shutdown: take the virtual monitor away so only real displays remain.
void RemoveVirtualMonitor() {
    if (g_monitorRemoved || !g_settings.removeMonitorOnExit || !g_settings.displayOverride.empty()) return;
    g_monitorRemoved = true;
    if (QueryVdd() != VddState::Ready) return;
    if (SetVirtualMonitorEnabled(false, 4000)) return;
    // No tasks (driver installed some other way): at least detach it from the desktop.
    VirtualDisplay vd;
    if (FindVirtualDisplay(L"", vd) && vd.attached) Detach(vd.gdiName);
}

void Balloon(const std::wstring& title, const std::wstring& text, DWORD icon = NIIF_INFO) {
    NOTIFYICONDATAW n = g_nid;
    n.uFlags = NIF_INFO;
    n.dwInfoFlags = icon;
    wcsncpy_s(n.szInfoTitle, title.c_str(), _TRUNCATE);
    wcsncpy_s(n.szInfo, text.c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &n);
}

std::wstring ExeDir() {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring s = exe;
    return s.substr(0, s.find_last_of(L'\\'));
}

// Runs "PadDisplay.exe --install-driver" elevated (one UAC prompt) on a worker thread.
void StartDriverInstall() {
    if (g_installing) return;
    g_installing = true;
    std::thread([] {
        wchar_t exe[MAX_PATH];
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        SHELLEXECUTEINFOW sei{sizeof(sei)};
        sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
        sei.lpVerb = L"runas";
        sei.lpFile = exe;
        sei.lpParameters = L"--install-driver";
        sei.nShow = SW_HIDE;
        DWORD code = 1;
        if (ShellExecuteExW(&sei) && sei.hProcess) {
            WaitForSingleObject(sei.hProcess, INFINITE);
            GetExitCodeProcess(sei.hProcess, &code);
            CloseHandle(sei.hProcess);
        } else {
            LOGI("driver install: elevation cancelled or failed (%lu)", GetLastError());
        }
        PostMessageW(g_hwnd, WM_DRIVER_DONE, code == 0, 0);
    }).detach();
}

int RunInstallDriver() {
    std::wstring message;
    bool ok = InstallVdd(ExeDir() + L"\\driver\\vdd", message);
    LOGI("install-driver: %s", Narrow(message).c_str());
    std::wstring gpu;
    ConfigureNvidiaProfile(g_settings.gpuMaxPerformance, gpu); // not fatal: streaming still works without it
    return ok ? 0 : 1;
}

// Setup steps that need the one-time admin run: on/off tasks, NVIDIA performance profile.
bool SetupIncomplete() {
    if (!ToggleTasksExist()) return true;
    return g_settings.gpuMaxPerformance && QueryNvidiaProfile() == GpuProfileState::NotSet;
}

HICON MakeIcon() {
    const int s = 32;
    BITMAPV5HEADER bh{};
    bh.bV5Size = sizeof(bh);
    bh.bV5Width = s;
    bh.bV5Height = -s;
    bh.bV5Planes = 1;
    bh.bV5BitCount = 32;
    bh.bV5Compression = BI_BITFIELDS;
    bh.bV5RedMask = 0x00FF0000;
    bh.bV5GreenMask = 0x0000FF00;
    bh.bV5BlueMask = 0x000000FF;
    bh.bV5AlphaMask = 0xFF000000;
    void* bits = nullptr;
    HDC dc = GetDC(nullptr);
    HBITMAP color = CreateDIBSection(dc, reinterpret_cast<BITMAPINFO*>(&bh), DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, dc);
    auto* px = static_cast<uint32_t*>(bits);
    for (int y = 0; y < s; ++y)
        for (int x = 0; x < s; ++x) {
            bool body = x >= 2 && x < 30 && y >= 6 && y < 26;   // tablet body
            bool screen = x >= 5 && x < 27 && y >= 9 && y < 23; // screen
            px[y * s + x] = !body ? 0 : screen ? 0xFF3DA5F5u : 0xFF263238u;
        }
    HBITMAP mask = CreateBitmap(s, s, 1, 1, nullptr);
    ICONINFO ii{TRUE, 0, 0, mask, color};
    HICON icon = CreateIconIndirect(&ii);
    DeleteObject(mask);
    DeleteObject(color);
    return icon;
}

bool EnsureGpuPreference(const std::wstring& gdiName);

void SetStatus(const std::wstring& s) {
    {
        std::lock_guard lock(g_statusMutex);
        g_status = s;
    }
    if (g_hwnd) PostMessageW(g_hwnd, WM_STATUS, 0, 0);
}

void ApplySettings() {
    g_settings.Save();
    if (g_server) g_server->UpdateSettings(g_settings);
}

constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

bool AutostartEnabled() {
    return RegGetValueW(HKEY_CURRENT_USER, kRunKey, L"PadDisplay", RRF_RT_REG_SZ, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
}

void SetAutostart(bool on) {
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS) return;
    if (on) {
        wchar_t exe[MAX_PATH];
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring cmd = L"\"" + std::wstring(exe) + L"\"";
        RegSetValueExW(key, L"PadDisplay", 0, REG_SZ, reinterpret_cast<const BYTE*>(cmd.c_str()), DWORD((cmd.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(key, L"PadDisplay");
    }
    RegCloseKey(key);
}

void ShowMenu(HWND hwnd) {
    HMENU m = CreatePopupMenu();
    std::wstring status;
    {
        std::lock_guard lock(g_statusMutex);
        status = g_status;
    }
    AppendMenuW(m, MF_STRING | MF_GRAYED, 0, L"PadDisplay " PD_VERSION_W L" by Nezucloud");
    AppendMenuW(m, MF_STRING | MF_GRAYED, 0, status.c_str());
    AppendMenuW(m, MF_STRING | MF_GRAYED, 0, (L"PIN: " + std::to_wstring(g_settings.pin)).c_str());
    VddState vdd = QueryVdd();
    if (g_installing) {
        AppendMenuW(m, MF_STRING | MF_GRAYED, 0, L"Installing virtual monitor driver…");
    } else if (vdd == VddState::Ready && g_setupIncomplete == 1) {
        AppendMenuW(m, MF_STRING, ID_DRIVER, L"Finish PadDisplay setup (admin)… (monitor off on exit, full GPU speed)");
    } else if (vdd != VddState::Ready) {
        AppendMenuW(m, MF_STRING | MF_DEFAULT, ID_DRIVER,
                    vdd == VddState::NotInstalled ? L"Install virtual monitor driver (admin)…"
                    : vdd == VddState::Disabled   ? L"Enable virtual monitor driver (admin)…"
                                                  : L"Repair virtual monitor driver (admin)…");
    }
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);

    auto rateMenu = [](UINT base, int current, std::initializer_list<int> rates) {
        HMENU sub = CreatePopupMenu();
        for (int r : rates)
            AppendMenuW(sub, MF_STRING | (r == current ? MF_CHECKED : 0), base + r, (std::to_wstring(r) + L" Mbps").c_str());
        return sub;
    };
    AppendMenuW(m, MF_POPUP, UINT_PTR(rateMenu(ID_USB_RATE, g_settings.usbBitrateMbps, {20, 30, 40, 60, 80})), L"Max bitrate (USB)");
    AppendMenuW(m, MF_POPUP, UINT_PTR(rateMenu(ID_WIFI_RATE, g_settings.wifiBitrateMbps, {8, 15, 25, 35, 50})), L"Max bitrate (Wi-Fi)");
    HMENU codec = CreatePopupMenu();
    AppendMenuW(codec, MF_STRING | (g_settings.codec == L"auto" ? MF_CHECKED : 0), ID_CODEC_AUTO, L"Auto (H.264 on USB, HEVC on Wi-Fi)");
    AppendMenuW(codec, MF_STRING | (g_settings.codec == L"hevc" ? MF_CHECKED : 0), ID_CODEC_HEVC, L"HEVC");
    AppendMenuW(codec, MF_STRING | (g_settings.codec == L"h264" ? MF_CHECKED : 0), ID_CODEC_H264, L"H.264");
    AppendMenuW(m, MF_POPUP, UINT_PTR(codec), L"Codec");
    AppendMenuW(m, MF_STRING | (g_settings.adaptive ? MF_CHECKED : 0), ID_ADAPTIVE, L"Adaptive bitrate");
    AppendMenuW(m, MF_STRING | (g_settings.drawCursor ? MF_CHECKED : 0), ID_CURSOR, L"Show mouse cursor on tablet");
    AppendMenuW(m, MF_STRING | (g_settings.matchClientResolution ? MF_CHECKED : 0), ID_MATCH_RES, L"Match tablet resolution");
    AppendMenuW(m, MF_STRING | (g_settings.detachOnDisconnect ? MF_CHECKED : 0), ID_DETACH, L"Remove virtual monitor when tablet disconnects");
    AppendMenuW(m, MF_STRING | MF_GRAYED, 0, L"(changes apply on next connection)");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | (AutostartEnabled() ? MF_CHECKED : 0), ID_AUTOSTART, L"Start with Windows");
    AppendMenuW(m, MF_STRING, ID_OPEN_LOG, L"Open log");
    AppendMenuW(m, MF_STRING, ID_OPEN_FOLDER, L"Open settings folder");
    AppendMenuW(m, MF_STRING, ID_EXIT, L"Exit");

    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(hwnd);
    TrackPopupMenu(m, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, nullptr);
    DestroyMenu(m);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    static UINT taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    if (msg == taskbarCreated) { // Explorer restarted: re-add the icon
        Shell_NotifyIconW(NIM_ADD, &g_nid);
        return 0;
    }
    switch (msg) {
    case WM_TRAY:
        if (LOWORD(lp) == WM_RBUTTONUP || LOWORD(lp) == WM_LBUTTONUP) ShowMenu(hwnd);
        else if (LOWORD(lp) == NIN_BALLOONUSERCLICK && QueryVdd() != VddState::Ready) StartDriverInstall();
        return 0;
    case WM_DRIVER_DONE: {
        g_installing = false;
        g_setupIncomplete = SetupIncomplete() ? 1 : 0;
        VddState s = QueryVdd();
        LOGI("driver install finished: %s, driver %ls", wp ? "ok" : "failed", VddStateText(s));
        if (wp && s == VddState::Ready) {
            Balloon(L"Virtual monitor ready", L"Connect the tablet to start using it as a second screen.");
            // Hybrid laptops: Desktop Duplication needs this process on the GPU that renders the new
            // monitor, and that preference only applies at start-up.
            VirtualDisplay vd;
            if (FindVirtualDisplay(g_settings.displayOverride, vd) && vd.attached && EnsureGpuPreference(vd.gdiName)) {
                g_restartAfterInstall = true;
                DestroyWindow(hwnd);
            }
        } else {
            Balloon(L"Virtual monitor driver not installed",
                    std::wstring(L"The driver is ") + VddStateText(s) + L". See the log (tray menu → Open log).", NIIF_WARNING);
        }
        return 0;
    }
    case WM_STATUS: {
        std::lock_guard lock(g_statusMutex);
        std::wstring tip = L"PadDisplay – " + g_status;
        wcsncpy_s(g_nid.szTip, tip.c_str(), _TRUNCATE);
        g_nid.uFlags = NIF_TIP;
        Shell_NotifyIconW(NIM_MODIFY, &g_nid);
        if (g_status.rfind(L"Error: ", 0) == 0) Balloon(L"PadDisplay", g_status.substr(7), NIIF_WARNING);
        return 0;
    }
    case WM_COMMAND: {
        UINT id = LOWORD(wp);
        if (id == ID_EXIT) DestroyWindow(hwnd);
        else if (id == ID_OPEN_LOG) ShellExecuteW(nullptr, L"open", LogPath().c_str(), nullptr, nullptr, SW_SHOW);
        else if (id == ID_OPEN_FOLDER) ShellExecuteW(nullptr, L"open", AppDataDir().c_str(), nullptr, nullptr, SW_SHOW);
        else if (id == ID_AUTOSTART) SetAutostart(!AutostartEnabled());
        else if (id == ID_DRIVER) StartDriverInstall();
        else {
            if (id == ID_ADAPTIVE) g_settings.adaptive = !g_settings.adaptive;
            else if (id == ID_CURSOR) g_settings.drawCursor = !g_settings.drawCursor;
            else if (id == ID_MATCH_RES) g_settings.matchClientResolution = !g_settings.matchClientResolution;
            else if (id == ID_DETACH) g_settings.detachOnDisconnect = !g_settings.detachOnDisconnect;
            else if (id == ID_CODEC_AUTO) g_settings.codec = L"auto";
            else if (id == ID_CODEC_HEVC) g_settings.codec = L"hevc";
            else if (id == ID_CODEC_H264) g_settings.codec = L"h264";
            else if (id > ID_WIFI_RATE && id < ID_WIFI_RATE + 100) g_settings.wifiBitrateMbps = int(id - ID_WIFI_RATE);
            else if (id > ID_USB_RATE && id < ID_USB_RATE + 100) g_settings.usbBitrateMbps = int(id - ID_USB_RATE);
            ApplySettings();
        }
        return 0;
    }
    case WM_QUERYENDSESSION:
        return TRUE;
    case WM_ENDSESSION:
        if (wp) { // Windows is signing out or shutting down: this process is about to be terminated.
            g_server.reset();
            RemoveVirtualMonitor();
        }
        return 0;
    case WM_DESTROY:
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// Desktop Duplication on hybrid laptops only works when this process runs on the GPU that renders
// the virtual monitor. Windows reads the per-app GPU preference at process start, so fix it and
// relaunch once if needed.
bool EnsureGpuPreference(const std::wstring& gdiName) {
    CaptureTarget t;
    if (gdiName.empty() || !FindOutput(gdiName, t)) return false;
    // 2 = high performance (discrete), 1 = power saving (integrated).
    const wchar_t* want = t.vendorId == 0x8086 ? L"GpuPreference=1;" : L"GpuPreference=2;";
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    HKEY key;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\DirectX\\UserGpuPreferences", 0, nullptr, 0,
                        KEY_READ | KEY_WRITE, nullptr, &key, nullptr) != ERROR_SUCCESS)
        return false;
    wchar_t cur[64] = {};
    DWORD size = sizeof(cur);
    bool same = RegGetValueW(key, nullptr, exe, RRF_RT_REG_SZ, nullptr, cur, &size) == ERROR_SUCCESS && wcscmp(cur, want) == 0;
    if (!same) {
        RegSetValueExW(key, exe, 0, REG_SZ, reinterpret_cast<const BYTE*>(want), DWORD((wcslen(want) + 1) * sizeof(wchar_t)));
        LOGI("gpu preference set to %ls for %ls (virtual display is on %ls)", want, exe, t.adapterName.c_str());
    }
    RegCloseKey(key);
    return !same;
}

bool HasArg(int argc, wchar_t** argv, const wchar_t* name) {
    for (int i = 1; i < argc; ++i)
        if (_wcsicmp(argv[i], name) == 0) return true;
    return false;
}

const wchar_t* ArgValue(int argc, wchar_t** argv, const wchar_t* name) {
    for (int i = 1; i + 1 < argc; ++i)
        if (_wcsicmp(argv[i], name) == 0) return argv[i + 1];
    return nullptr;
}

int RunDump(const wchar_t* path, int argc, wchar_t** argv) {
    ClientHello h;
    h.version = kProtocolVersion;
    h.screenW = 2560;
    h.screenH = 1600;
    h.codecMask = MaskH264 | MaskHEVC;
    strcpy_s(h.name, "dump");
    if (auto size = ArgValue(argc, argv, L"--size")) swscanf_s(size, L"%hux%hu", &h.screenW, &h.screenH);
    Settings s = g_settings;
    if (auto c = ArgValue(argc, argv, L"--codec")) s.codec = c;
    int seconds = 10;
    if (auto sec = ArgValue(argc, argv, L"--seconds")) seconds = _wtoi(sec);
    if (auto fps = ArgValue(argc, argv, L"--fps")) h.maxFps = uint16_t(_wtoi(fps));

    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"wb") || !f) {
        LOGI("cannot open %ls", path);
        return 1;
    }
    Session session(INVALID_SOCKET, h, true, s, [](const std::wstring& st) { LOGI("%s", Narrow(st).c_str()); });
    session.Run(f, seconds);
    long size = ftell(f);
    fclose(f);
    LOGI("dump: wrote %ld bytes to %ls", size, path);
    return size > 0 ? 0 : 1;
}

} // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    DisableProcessPowerThrottling();
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    const wchar_t* dumpPath = ArgValue(argc, argv, L"--dump");
    bool list = HasArg(argc, argv, L"--list");
    bool installDriver = HasArg(argc, argv, L"--install-driver");
    bool console = HasArg(argc, argv, L"--console") || list || dumpPath;

    // Before LogInit, which truncates host.log: a second copy must not wipe the running one's log.
    HANDLE single = nullptr;
    if (!dumpPath && !list && !installDriver) {
        single = CreateMutexW(nullptr, TRUE, L"Local\\PadDisplayHost");
        if (GetLastError() == ERROR_ALREADY_EXISTS) {
            MessageBoxW(nullptr, L"PadDisplay is already running (see the tray).", L"PadDisplay", MB_ICONINFORMATION);
            return 0;
        }
    }

    LogInit(AppDataDir() + (installDriver ? L"\\install-driver.log" : dumpPath || list ? L"\\host-cli.log" : L"\\host.log"), console);
    g_settings.Load();
    if (installDriver) return RunInstallDriver();
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION, MFSTARTUP_LITE);
    NetStartup();

    if (list) {
        LogDisplays();
        VirtualDisplay vd;
        if (FindVirtualDisplay(g_settings.displayOverride, vd)) {
            LOGI("virtual display: %ls (%ls) attached=%d", vd.gdiName.c_str(), vd.adapterName.c_str(), vd.attached);
            for (auto& m : ListModes(vd.gdiName)) LOGI("    mode %dx%d@%d", m.width, m.height, m.hz);
        } else {
            LOGI("virtual display: not found");
        }
        return 0;
    }

    if (QueryVdd() != VddState::Ready && LoadHomeLayout().empty()) SaveHomeLayout(SnapshotRealDisplays());
    // The virtual monitor is switched off while PadDisplay is not running; switch it back on.
    if (g_settings.displayOverride.empty() && QueryVdd() == VddState::Disabled && SetVirtualMonitorEnabled(true, 5000)) {
        VirtualDisplay tmp;
        for (int i = 0; i < 30 && !FindVirtualDisplay(L"", tmp); ++i) Sleep(100);
    }
    VirtualDisplay vd;
    bool haveVd = FindVirtualDisplay(g_settings.displayOverride, vd);
    if (haveVd && vd.attached && EnsureGpuPreference(vd.gdiName)) {
        // Relaunch so the new GPU preference takes effect.
        STARTUPINFOW si{sizeof(si)};
        PROCESS_INFORMATION pi{};
        std::wstring cmd = GetCommandLineW();
        ReleaseMutex(single);
        CloseHandle(single);
        if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            return 0;
        }
    }
    VddState vddState = QueryVdd();
    if (!haveVd) LOGI("warning: no virtual display found (driver %ls)", VddStateText(vddState));

    if (dumpPath) {
        return RunDump(dumpPath, argc, argv);
    }

    g_server = std::make_unique<Server>(g_settings, SetStatus);
    if (!g_server->Start()) {
        MessageBoxW(nullptr, L"PadDisplay could not listen on TCP port 27183. Is it already running?", L"PadDisplay", MB_ICONERROR);
        return 1;
    }
    DiscoveryBeacon beacon;
    beacon.Start();
    AdbReverser adb;
    adb.Start(g_settings.adbPath);

    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.lpszClassName = L"PadDisplayTray";
    RegisterClassW(&wc);
    g_hwnd = CreateWindowW(wc.lpszClassName, L"PadDisplay", 0, 0, 0, 0, 0, nullptr, nullptr, inst, nullptr);
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = g_hwnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAY;
    g_nid.hIcon = MakeIcon();
    wcscpy_s(g_nid.szTip, L"PadDisplay – waiting for tablet");
    Shell_NotifyIconW(NIM_ADD, &g_nid);
    LOGI("PadDisplay %s by Nezucloud started. PIN %u", PD_VERSION_STR, g_settings.pin);
    std::thread([] {
        g_setupIncomplete = SetupIncomplete() ? 1 : 0;
        LOGI("setup: %s; NVIDIA profile %s", g_setupIncomplete ? "incomplete (tray: Finish PadDisplay setup)" : "complete",
             QueryNvidiaProfile() == GpuProfileState::Set ? "max performance" : "not set");
    }).detach();
    if (vddState != VddState::Ready && g_settings.displayOverride.empty())
        Balloon(vddState == VddState::NotInstalled ? L"Virtual monitor driver needed" : L"Virtual monitor driver is off",
                L"Click here to set it up (one administrator prompt). PadDisplay needs it to add the tablet as a second screen.",
                NIIF_WARNING);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    adb.Stop();
    beacon.Stop();
    g_server.reset();
    if (!g_restartAfterInstall) RemoveVirtualMonitor();
    MFShutdown();
    if (g_restartAfterInstall) {
        ReleaseMutex(single);
        CloseHandle(single);
        STARTUPINFOW si{sizeof(si)};
        PROCESS_INFORMATION pi{};
        std::wstring cmd = GetCommandLineW();
        if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        }
    }
    return 0;
}
