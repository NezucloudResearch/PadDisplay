// PadDisplay by Nezucloud. Copyright (c) 2026 Nezucloud.
// SPDX-License-Identifier: MIT
#include "vdd.h"
#include "display.h"
#include "log.h"
#include "net.h"

#include <windows.h>
#include <cfgmgr32.h>
#include <dxgi.h>
#include <newdev.h>
#include <setupapi.h>
#include <wrl/client.h>
#include <cwctype>
#include <fstream>
#include <regex>
#include <sstream>
#include <vector>

namespace pd {
namespace {

// {4d36e968-e325-11ce-bfc1-08002be10318}
const GUID kDisplayClass = {0x4d36e968, 0xe325, 0x11ce, {0xbf, 0xc1, 0x08, 0x00, 0x2b, 0xe1, 0x03, 0x18}};
const wchar_t kHardwareId[] = L"Root\\MttVDD";
const wchar_t kSettingsDir[] = L"C:\\VirtualDisplayDriver";
const wchar_t kTaskOn[] = L"\\PadDisplay\\VirtualMonitorOn";
const wchar_t kTaskOff[] = L"\\PadDisplay\\VirtualMonitorOff";

// Common tablet / monitor resolutions, landscape. The tablet's own mode is added on connect.
const int kDefaultModes[][2] = {
    {1280, 800},  {1920, 1080}, {1920, 1200}, {2000, 1200}, {2160, 1440}, {2240, 1400}, {2388, 1668},
    {2560, 1440}, {2560, 1600}, {2800, 1752}, {2880, 1800}, {2960, 1848}, {3000, 2000},
};
const int kDefaultRates[] = {60, 90, 120, 144};

bool HasHardwareId(HDEVINFO set, SP_DEVINFO_DATA& d) {
    wchar_t buf[1024] = {};
    if (!SetupDiGetDeviceRegistryPropertyW(set, &d, SPDRP_HARDWAREID, nullptr, reinterpret_cast<BYTE*>(buf), sizeof(buf) - 4, nullptr))
        return false;
    for (const wchar_t* p = buf; *p; p += wcslen(p) + 1)
        if (_wcsicmp(p, kHardwareId) == 0) return true;
    return false;
}

// Finds the VDD device node (present or not). Returns false if there is none.
bool FindDevice(HDEVINFO set, SP_DEVINFO_DATA& out) {
    SP_DEVINFO_DATA d{sizeof(d)};
    for (DWORD i = 0; SetupDiEnumDeviceInfo(set, i, &d); ++i) {
        if (HasHardwareId(set, d)) {
            out = d;
            return true;
        }
    }
    return false;
}

std::wstring GpuFriendlyName() {
    // Prefer the discrete GPU so NVENC/AMF can encode the virtual monitor without cross-adapter copies.
    Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return L"default";
    std::wstring best = L"default";
    SIZE_T bestMem = 0;
    int hardware = 0;
    Microsoft::WRL::ComPtr<IDXGIAdapter1> a;
    for (UINT i = 0; factory->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 d;
        a->GetDesc1(&d);
        if ((d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) || d.VendorId == 0x1414) continue;
        ++hardware;
        if (d.DedicatedVideoMemory > bestMem) {
            bestMem = d.DedicatedVideoMemory;
            best = d.Description;
        }
    }
    return hardware > 1 ? best : L"default"; // single GPU: let the driver pick
}

std::string DefaultSettingsXml() {
    std::ostringstream x;
    x << "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<vdd_settings>\n  <monitors>\n    <count>1</count>\n  </monitors>\n"
      << "  <gpu>\n    <friendlyname>" << Narrow(GpuFriendlyName()) << "</friendlyname>\n  </gpu>\n  <global>\n";
    for (int r : kDefaultRates) x << "    <g_refresh_rate>" << r << "</g_refresh_rate>\n";
    x << "  </global>\n  <resolutions>\n";
    for (auto& m : kDefaultModes)
        x << "    <resolution>\n      <width>" << m[0] << "</width>\n      <height>" << m[1]
          << "</height>\n      <refresh_rate>60</refresh_rate>\n    </resolution>\n";
    x << "  </resolutions>\n  <options>\n    <CustomEdid>false</CustomEdid>\n    <PreventSpoof>false</PreventSpoof>\n"
      << "    <EdidCeaOverride>false</EdidCeaOverride>\n    <HardwareCursor>false</HardwareCursor>\n"
      << "    <SDR10bit>false</SDR10bit>\n    <HDRPlus>false</HDRPlus>\n    <logging>false</logging>\n"
      << "    <debuglogging>false</debuglogging>\n  </options>\n</vdd_settings>\n";
    return x.str();
}

bool ReadFile(const std::wstring& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::ostringstream s;
    s << f.rdbuf();
    out = s.str();
    return true;
}

bool WriteFileAtomic(const std::wstring& path, const std::string& data) {
    std::wstring tmp = path + L".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f || !f.write(data.data(), std::streamsize(data.size()))) return false;
    }
    return MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != FALSE;
}

bool EnsureSettingsFile() {
    std::wstring path = VddSettingsPath();
    if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) return true;
    CreateDirectoryW(kSettingsDir, nullptr);
    bool ok = WriteFileAtomic(path, DefaultSettingsXml());
    LOGI("vdd: %s default %ls", ok ? "wrote" : "FAILED to write", path.c_str());
    return ok;
}

// Tells the running driver to re-read vdd_settings.xml. The pipe is open to normal users.
bool ReloadDriver() {
    HANDLE pipe = INVALID_HANDLE_VALUE;
    for (int i = 0; i < 20 && pipe == INVALID_HANDLE_VALUE; ++i) {
        pipe = CreateFileW(L"\\\\.\\pipe\\MTTVirtualDisplayPipe", GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (pipe == INVALID_HANDLE_VALUE) {
            if (GetLastError() != ERROR_PIPE_BUSY || !WaitNamedPipeW(L"\\\\.\\pipe\\MTTVirtualDisplayPipe", 500)) Sleep(100);
        }
    }
    if (pipe == INVALID_HANDLE_VALUE) {
        LOGI("vdd: control pipe unavailable (%lu)", GetLastError());
        return false;
    }
    const wchar_t cmd[] = L"RELOAD_DRIVER";
    DWORD written = 0;
    BOOL ok = WriteFile(pipe, cmd, DWORD(wcslen(cmd) * sizeof(wchar_t)), &written, nullptr);
    CloseHandle(pipe);
    LOGI("vdd: reload requested (%s)", ok ? "ok" : "failed");
    return ok != FALSE;
}

bool ChangeState(HDEVINFO set, SP_DEVINFO_DATA& d, DWORD state, DWORD scope) {
    SP_PROPCHANGE_PARAMS p{};
    p.ClassInstallHeader.cbSize = sizeof(SP_CLASSINSTALL_HEADER);
    p.ClassInstallHeader.InstallFunction = DIF_PROPERTYCHANGE;
    p.StateChange = state;
    p.Scope = scope;
    return SetupDiSetClassInstallParamsW(set, &d, &p.ClassInstallHeader, sizeof(p)) &&
           SetupDiCallClassInstaller(DIF_PROPERTYCHANGE, set, &d);
}

std::wstring LastErrorText(DWORD err) {
    wchar_t* msg = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, err, 0,
                   reinterpret_cast<wchar_t*>(&msg), 0, nullptr);
    std::wstring s = msg ? msg : L"";
    LocalFree(msg);
    while (!s.empty() && (s.back() == L'\n' || s.back() == L'\r' || s.back() == L'.')) s.pop_back();
    return s + L" (" + std::to_wstring(err) + L")";
}

// Registers a hidden task that runs "pnputil /<verb>-device /deviceid Root\MttVDD" as SYSTEM (no
// window). The security descriptor lets interactive users start it; it can only toggle this device.
bool CreateToggleTask(const wchar_t* name, const wchar_t* verb, std::wstring& message) {
    std::wstring xml =
        L"<?xml version=\"1.0\" encoding=\"UTF-16\"?>\r\n"
        L"<Task version=\"1.2\" xmlns=\"http://schemas.microsoft.com/windows/2004/02/mit/task\">\r\n"
        L"  <RegistrationInfo>\r\n"
        L"    <Author>PadDisplay</Author>\r\n"
        L"    <Description>PadDisplay: " + std::wstring(verb) + L" the virtual monitor (Root\\MttVDD).</Description>\r\n"
        L"    <SecurityDescriptor>D:(A;;FA;;;SY)(A;;FA;;;BA)(A;;GRGX;;;IU)</SecurityDescriptor>\r\n"
        L"  </RegistrationInfo>\r\n"
        L"  <Principals><Principal id=\"System\"><UserId>S-1-5-18</UserId><RunLevel>HighestAvailable</RunLevel></Principal></Principals>\r\n"
        L"  <Settings>\r\n"
        L"    <MultipleInstancesPolicy>Queue</MultipleInstancesPolicy>\r\n"
        L"    <DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries>\r\n"
        L"    <StopIfGoingOnBatteries>false</StopIfGoingOnBatteries>\r\n"
        L"    <AllowStartOnDemand>true</AllowStartOnDemand>\r\n"
        L"    <Hidden>true</Hidden>\r\n"
        L"    <ExecutionTimeLimit>PT1M</ExecutionTimeLimit>\r\n"
        L"    <Priority>4</Priority>\r\n"
        L"  </Settings>\r\n"
        L"  <Actions Context=\"System\"><Exec>\r\n"
        L"    <Command>%SystemRoot%\\System32\\pnputil.exe</Command>\r\n"
        L"    <Arguments>/" + std::wstring(verb) + L"-device /deviceid \"Root\\MttVDD\"</Arguments>\r\n"
        L"  </Exec></Actions>\r\n"
        L"</Task>\r\n";
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    std::wstring path = std::wstring(tmp) + L"paddisplay-task.xml";
    {
        std::ofstream f(path, std::ios::binary | std::ios::trunc);
        const unsigned char bom[2] = {0xFF, 0xFE};
        f.write(reinterpret_cast<const char*>(bom), 2);
        f.write(reinterpret_cast<const char*>(xml.data()), std::streamsize(xml.size() * sizeof(wchar_t)));
    }
    DWORD code = 1;
    std::string out = RunCapture(L"schtasks.exe /create /f /tn \"" + std::wstring(name) + L"\" /xml \"" + path + L"\"", 15000, &code);
    DeleteFileW(path.c_str());
    if (code != 0) {
        message = L"Could not register task " + std::wstring(name) + L": " + Widen(out);
        LOGI("vdd: %s", Narrow(message).c_str());
        return false;
    }
    return true;
}

bool InstallToggleTasks(std::wstring& message) {
    bool ok = CreateToggleTask(kTaskOff, L"disable", message) && CreateToggleTask(kTaskOn, L"enable", message);
    LOGI("vdd: on/off tasks %s", ok ? "registered" : "FAILED");
    return ok;
}

bool InstallDriver(const std::wstring& driverDir, std::wstring& message);

} // namespace

bool ToggleTasksExist() {
    DWORD a = 1, b = 1;
    RunCapture(L"schtasks.exe /query /tn \"" + std::wstring(kTaskOn) + L"\"", 10000, &a);
    RunCapture(L"schtasks.exe /query /tn \"" + std::wstring(kTaskOff) + L"\"", 10000, &b);
    return a == 0 && b == 0;
}

bool SetVirtualMonitorEnabled(bool on, unsigned timeoutMs) {
    VddState want = on ? VddState::Ready : VddState::Disabled;
    if (QueryVdd() == want) return true;
    // While the virtual monitor is off, the current arrangement is the user's own: remember it.
    if (on) SaveHomeLayout(SnapshotRealDisplays());
    DWORD code = 1;
    RunCapture(L"schtasks.exe /run /tn \"" + std::wstring(on ? kTaskOn : kTaskOff) + L"\"", 10000, &code);
    if (code != 0) {
        LOGI("vdd: cannot start the %s task (%lu)", on ? "on" : "off", code);
        return false;
    }
    ULONGLONG deadline = GetTickCount64() + timeoutMs;
    while (GetTickCount64() < deadline) {
        if (QueryVdd() == want) {
            LOGI("vdd: virtual monitor %s", on ? "enabled" : "removed");
            // Windows re-applies its stored layout for the new monitor set a moment later; if that
            // switched off or moved a real screen, put it back where the user had it.
            RestoreHomeLayout();
            return true;
        }
        Sleep(100);
    }
    LOGI("vdd: virtual monitor did not %s in time", on ? "come back" : "go away");
    return false;
}

std::wstring VddSettingsPath() { return std::wstring(kSettingsDir) + L"\\vdd_settings.xml"; }

const wchar_t* VddStateText(VddState s) {
    switch (s) {
    case VddState::NotInstalled: return L"not installed";
    case VddState::Disabled: return L"disabled";
    case VddState::Problem: return L"not working";
    default: return L"ready";
    }
}

VddState QueryVdd(std::wstring* detail) {
    HDEVINFO set = SetupDiGetClassDevsW(&kDisplayClass, nullptr, nullptr, 0); // include non-present devices
    if (set == INVALID_HANDLE_VALUE) return VddState::NotInstalled;
    VddState state = VddState::NotInstalled;
    SP_DEVINFO_DATA d{sizeof(d)};
    if (FindDevice(set, d)) {
        ULONG status = 0, problem = 0;
        CONFIGRET cr = CM_Get_DevNode_Status(&status, &problem, d.DevInst, 0);
        if (cr != CR_SUCCESS) state = VddState::NotInstalled; // leftover, non-present device node
        else if (problem == CM_PROB_DISABLED) state = VddState::Disabled;
        else if ((status & DN_HAS_PROBLEM) || !(status & DN_STARTED)) state = VddState::Problem;
        else state = VddState::Ready;
        if (detail) *detail = L"status 0x" + std::to_wstring(status) + L", problem " + std::to_wstring(problem);
    }
    SetupDiDestroyDeviceInfoList(set);
    return state;
}

bool InstallVdd(const std::wstring& driverDir, std::wstring& message) {
    if (!InstallDriver(driverDir, message)) return false;
    std::wstring taskMessage;
    if (!InstallToggleTasks(taskMessage)) message += L" (Removing it on exit will not work: " + taskMessage + L")";
    return true;
}

namespace {
bool InstallDriver(const std::wstring& driverDir, std::wstring& message) {
    std::wstring inf = driverDir + L"\\MttVDD.inf";
    if (GetFileAttributesW(inf.c_str()) == INVALID_FILE_ATTRIBUTES) {
        message = L"Driver package not found: " + inf;
        return false;
    }
    if (!EnsureSettingsFile()) {
        message = L"Could not write " + VddSettingsPath();
        return false;
    }

    HDEVINFO set = SetupDiGetClassDevsW(&kDisplayClass, nullptr, nullptr, 0);
    SP_DEVINFO_DATA d{sizeof(d)};
    if (set != INVALID_HANDLE_VALUE && FindDevice(set, d)) {
        ULONG status = 0, problem = 0;
        if (CM_Get_DevNode_Status(&status, &problem, d.DevInst, 0) == CR_SUCCESS) {
            if (problem == CM_PROB_DISABLED) {
                bool ok = ChangeState(set, d, DICS_ENABLE, DICS_FLAG_GLOBAL) || ChangeState(set, d, DICS_ENABLE, DICS_FLAG_CONFIGSPECIFIC);
                LOGI("vdd: enable existing device -> %s", ok ? "ok" : Narrow(LastErrorText(GetLastError())).c_str());
            } else if (status & DN_HAS_PROBLEM) {
                ChangeState(set, d, DICS_PROPCHANGE, DICS_FLAG_GLOBAL); // restart it
            }
            SetupDiDestroyDeviceInfoList(set);
            // Make sure the driver itself is the bundled (or newer) version.
            BOOL reboot = FALSE;
            UpdateDriverForPlugAndPlayDevicesW(nullptr, kHardwareId, inf.c_str(), 0, &reboot);
            for (int i = 0; i < 30 && QueryVdd() != VddState::Ready; ++i) Sleep(200);
            VddState s = QueryVdd();
            message = s == VddState::Ready ? L"Virtual monitor driver enabled." : std::wstring(L"Driver is ") + VddStateText(s);
            return s == VddState::Ready;
        }
    }
    if (set != INVALID_HANDLE_VALUE) SetupDiDestroyDeviceInfoList(set);

    // Create the root-enumerated device (like "devcon install"), then bind the driver to it.
    set = SetupDiCreateDeviceInfoList(&kDisplayClass, nullptr);
    if (set == INVALID_HANDLE_VALUE) {
        message = L"SetupDiCreateDeviceInfoList: " + LastErrorText(GetLastError());
        return false;
    }
    d = {sizeof(d)};
    const wchar_t hwid[] = L"Root\\MttVDD\0"; // REG_MULTI_SZ: ends with two NULs
    bool ok = SetupDiCreateDeviceInfoW(set, L"Display", &kDisplayClass, L"Virtual Display Driver", nullptr, DICD_GENERATE_ID, &d) &&
              SetupDiSetDeviceRegistryPropertyW(set, &d, SPDRP_HARDWAREID, reinterpret_cast<const BYTE*>(hwid), sizeof(hwid)) &&
              SetupDiCallClassInstaller(DIF_REGISTERDEVICE, set, &d);
    if (!ok) {
        message = L"Could not create the device: " + LastErrorText(GetLastError());
        SetupDiDestroyDeviceInfoList(set);
        return false;
    }
    BOOL reboot = FALSE;
    if (!UpdateDriverForPlugAndPlayDevicesW(nullptr, kHardwareId, inf.c_str(), INSTALLFLAG_FORCE, &reboot)) {
        DWORD err = GetLastError();
        SetupDiCallClassInstaller(DIF_REMOVE, set, &d);
        SetupDiDestroyDeviceInfoList(set);
        message = L"Driver install failed: " + LastErrorText(err);
        LOGI("vdd: %s", Narrow(message).c_str());
        return false;
    }
    SetupDiDestroyDeviceInfoList(set);
    for (int i = 0; i < 30 && QueryVdd() != VddState::Ready; ++i) Sleep(200);
    VddState s = QueryVdd();
    message = s == VddState::Ready ? (reboot ? L"Virtual monitor driver installed. Restart Windows to finish."
                                             : L"Virtual monitor driver installed.")
                                   : std::wstring(L"Installed, but the driver is ") + VddStateText(s);
    LOGI("vdd: %s", Narrow(message).c_str());
    return s == VddState::Ready;
}
} // namespace

bool EnsureVddMode(int width, int height, int hz, bool& changed) {
    changed = false;
    if (!EnsureSettingsFile()) return false;
    std::string xml;
    if (!ReadFile(VddSettingsPath(), xml)) return false;

    std::string w = std::to_string(width), h = std::to_string(height), r = std::to_string(hz);
    std::regex hasMode("<width>\\s*" + w + "\\s*</width>\\s*<height>\\s*" + h + "\\s*</height>");
    std::regex hasRate("<g_refresh_rate>\\s*" + r + "\\s*</g_refresh_rate>");
    if (!std::regex_search(xml, hasMode)) {
        size_t at = xml.find("</resolutions>");
        if (at == std::string::npos) return false;
        xml.insert(at, "  <resolution>\n      <width>" + w + "</width>\n      <height>" + h +
                           "</height>\n      <refresh_rate>60</refresh_rate>\n    </resolution>\n  ");
        changed = true;
    }
    if (hz > 0 && !std::regex_search(xml, hasRate)) {
        size_t at = xml.find("</global>");
        if (at == std::string::npos) return false;
        xml.insert(at, "  <g_refresh_rate>" + r + "</g_refresh_rate>\n  ");
        changed = true;
    }
    if (!changed) return true;
    if (!WriteFileAtomic(VddSettingsPath(), xml)) {
        LOGI("vdd: cannot write %ls (%lu)", VddSettingsPath().c_str(), GetLastError());
        changed = false;
        return false;
    }
    LOGI("vdd: added %dx%d@%d to vdd_settings.xml", width, height, hz);
    return ReloadDriver();
}

} // namespace pd
