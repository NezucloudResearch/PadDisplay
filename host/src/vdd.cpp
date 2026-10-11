// PadDisplay by Nezucloud. Copyright (c) 2026 Nezucloud.
// SPDX-License-Identifier: MIT
#include "vdd.h"
#include "cablesink.h"
#include "display.h"
#include "log.h"
#include "net.h"
#include "setup.h"

#include "../third_party/padmic/include/padmic_public.h"

#include <windows.h>
#include <cfgmgr32.h>
#include <dxgi.h>
#include <newdev.h>
#include <setupapi.h>
#include <softpub.h>
#include <wintrust.h>
#include <wincrypt.h>
#include <bcrypt.h>
#include <wrl/client.h>
#include <atomic>
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
// {4d36e97d-e325-11ce-bfc1-08002be10318}: ViGEmBus, the virtual game controller bus, is a System device.
const GUID kSystemClass = {0x4d36e97d, 0xe325, 0x11ce, {0xbf, 0xc1, 0x08, 0x00, 0x2b, 0xe1, 0x03, 0x18}};
const wchar_t kVigemHardwareId[] = L"Nefarius\\ViGEmBus\\Gen1";
const wchar_t kTaskGamepadOn[] = L"\\PadDisplay\\GameControllerOn";
// {4d36e96c-e325-11ce-bfc1-08002be10318}: MEDIA, where audio devices live. The PadDisplay Microphone is a root device.
const GUID kMediaClass = {0x4d36e96c, 0xe325, 0x11ce, {0xbf, 0xc1, 0x08, 0x00, 0x2b, 0xe1, 0x03, 0x18}};
const wchar_t kMicHardwareId[] = L"Root\\PadDisplayMic";
const wchar_t kTaskMicOn[] = L"\\PadDisplay\\MicrophoneOn";
const wchar_t kTaskMicOff[] = L"\\PadDisplay\\MicrophoneOff";

// Common tablet / monitor resolutions, landscape. The tablet's own mode is added on connect.
const int kDefaultModes[][2] = {
    {1280, 800},  {1920, 1080}, {1920, 1200}, {2000, 1200}, {2160, 1440}, {2240, 1400}, {2388, 1668},
    {2560, 1440}, {2560, 1600}, {2800, 1752}, {2880, 1800}, {2960, 1848}, {3000, 2000},
};
const int kDefaultRates[] = {60, 90, 120, 144};

bool HasHardwareId(HDEVINFO set, SP_DEVINFO_DATA& d, const wchar_t* hardwareId) {
    wchar_t buf[1024] = {};
    if (!SetupDiGetDeviceRegistryPropertyW(set, &d, SPDRP_HARDWAREID, nullptr, reinterpret_cast<BYTE*>(buf), sizeof(buf) - 4, nullptr))
        return false;
    for (const wchar_t* p = buf; *p; p += wcslen(p) + 1)
        if (_wcsicmp(p, hardwareId) == 0) return true;
    return false;
}

// Finds the device node with this hardware ID (present or not). Returns false if there is none.
bool FindDevice(HDEVINFO set, SP_DEVINFO_DATA& out, const wchar_t* hardwareId = kHardwareId) {
    SP_DEVINFO_DATA d{sizeof(d)};
    for (DWORD i = 0; SetupDiEnumDeviceInfo(set, i, &d); ++i) {
        if (HasHardwareId(set, d, hardwareId)) {
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

// wrote (optional) reports whether the file was written now, as opposed to having been there.
bool EnsureSettingsFile(bool* wrote = nullptr) {
    std::wstring path = VddSettingsPath();
    if (wrote) *wrote = false;
    if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) return true;
    CreateDirectoryW(kSettingsDir, nullptr);
    bool ok = WriteFileAtomic(path, DefaultSettingsXml());
    LOGI("vdd: %s default %ls", ok ? "wrote" : "FAILED to write", path.c_str());
    if (wrote) *wrote = ok;
    return ok;
}

void DeleteSettingsFile() {
    DeleteFileW(VddSettingsPath().c_str());
    RemoveDirectoryW(kSettingsDir); // fails, as it should, if something else keeps files there
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

// SetupAPI's own error codes have no message text in Windows: say what the one a user can cause means.
std::wstring InstallErrorText(DWORD err) {
    if (err == ERROR_AUTHENTICODE_PUBLISHER_NOT_TRUSTED || err == ERROR_AUTHENTICODE_TRUST_NOT_ESTABLISHED)
        return L"Windows asked whether to install software from the driver's publisher, and the answer was no (" + std::to_wstring(err) + L")";
    return LastErrorText(err);
}

// Registers a hidden task that runs "pnputil /<verb>-device /deviceid <hardwareId>" as SYSTEM (no
// window). The security descriptor lets interactive users start it; it can only toggle this device.
bool CreateToggleTask(const wchar_t* name, const wchar_t* verb, const wchar_t* what, const wchar_t* hardwareId, std::wstring& message) {
    std::wstring xml =
        L"<?xml version=\"1.0\" encoding=\"UTF-16\"?>\r\n"
        L"<Task version=\"1.2\" xmlns=\"http://schemas.microsoft.com/windows/2004/02/mit/task\">\r\n"
        L"  <RegistrationInfo>\r\n"
        L"    <Author>PadDisplay</Author>\r\n"
        L"    <Description>PadDisplay: " + std::wstring(verb) + L" the " + what + L" (" + hardwareId + L").</Description>\r\n"
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
        L"    <Arguments>/" + std::wstring(verb) + L"-device /deviceid \"" + hardwareId + L"\"</Arguments>\r\n"
        L"  </Exec></Actions>\r\n"
        L"</Task>\r\n";
    return CreateTaskFromXml(name, xml, message);
}

bool InstallToggleTasks(std::wstring& message) {
    bool ok = CreateToggleTask(kTaskOff, L"disable", L"virtual monitor", kHardwareId, message) &&
              CreateToggleTask(kTaskOn, L"enable", L"virtual monitor", kHardwareId, message);
    LOGI("vdd: on/off tasks %s", ok ? "registered" : "FAILED");
    return ok;
}

// Removes the device node with this hardware ID. false = there was none, or Windows refused.
bool RemoveDevice(const GUID& deviceClass, const wchar_t* hardwareId) {
    bool removed = false;
    HDEVINFO set = SetupDiGetClassDevsW(&deviceClass, nullptr, nullptr, 0);
    SP_DEVINFO_DATA d{sizeof(d)};
    if (set != INVALID_HANDLE_VALUE && FindDevice(set, d, hardwareId)) removed = SetupDiCallClassInstaller(DIF_REMOVE, set, &d) != FALSE;
    if (set != INVALID_HANDLE_VALUE) SetupDiDestroyDeviceInfoList(set);
    return removed;
}

void DeleteTask(const wchar_t* name) {
    DWORD ignored = 0;
    RunCapture(L"schtasks.exe /delete /f /tn \"" + std::wstring(name) + L"\"", 10000, &ignored);
}

// A driver package stays in the driver store until it is deleted: finds the oemNN.inf files by the hardware ID they install
// (given in lower case) and deletes them. Returns how many.
int DeleteDriverPackages(const char* hardwareIdLower) {
    int packages = 0;
    wchar_t windir[MAX_PATH];
    GetWindowsDirectoryW(windir, MAX_PATH);
    WIN32_FIND_DATAW fd;
    HANDLE find = FindFirstFileW((std::wstring(windir) + L"\\INF\\oem*.inf").c_str(), &fd);
    if (find == INVALID_HANDLE_VALUE) return 0;
    do {
        std::string text;
        if (!ReadFile(std::wstring(windir) + L"\\INF\\" + fd.cFileName, text)) continue;
        // An INF is ANSI or UTF-16 (the virtual monitor's is UTF-16, and Windows stores it as it came).
        if (text.size() >= 2 && static_cast<unsigned char>(text[0]) == 0xFF && static_cast<unsigned char>(text[1]) == 0xFE)
            text = Narrow(std::wstring(reinterpret_cast<const wchar_t*>(text.data() + 2), (text.size() - 2) / sizeof(wchar_t)));
        for (auto& c : text) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
        if (text.find(hardwareIdLower) != std::string::npos && SetupUninstallOEMInfW(fd.cFileName, SUOI_FORCEDELETE, nullptr)) ++packages;
    } while (FindNextFileW(find, &fd));
    FindClose(find);
    return packages;
}

bool InstallDriver(const std::wstring& driverDir, std::wstring& message);

} // namespace

bool CreateTaskFromXml(const wchar_t* name, const std::wstring& xml, std::wstring& message) {
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

bool ToggleTasksExist() {
    DWORD a = 1, b = 1;
    RunCapture(L"schtasks.exe /query /tn \"" + std::wstring(kTaskOn) + L"\"", 10000, &a);
    RunCapture(L"schtasks.exe /query /tn \"" + std::wstring(kTaskOff) + L"\"", 10000, &b);
    return a == 0 && b == 0;
}

namespace {
std::atomic<bool> vddFailed{false};
}

void SetVddFailed(bool failed) { vddFailed = failed; }
bool VddFailed() { return vddFailed; }

bool SetVirtualMonitorEnabled(bool on, unsigned timeoutMs) {
    VddState want = on ? VddState::Ready : VddState::Disabled;
    if (QueryVdd() == want) return true;
    // While the virtual monitor is off, the current arrangement is the user's own: remember it.
    if (on) RememberRealScreens();
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
            RestoreRealScreens();
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

static VddState QueryDevice(const GUID& deviceClass, const wchar_t* hardwareId, std::wstring* detail, ULONG* problemOut = nullptr) {
    HDEVINFO set = SetupDiGetClassDevsW(&deviceClass, nullptr, nullptr, 0); // include non-present devices
    if (set == INVALID_HANDLE_VALUE) return VddState::NotInstalled;
    VddState state = VddState::NotInstalled;
    SP_DEVINFO_DATA d{sizeof(d)};
    if (FindDevice(set, d, hardwareId)) {
        ULONG status = 0, problem = 0;
        CONFIGRET cr = CM_Get_DevNode_Status(&status, &problem, d.DevInst, 0);
        if (cr != CR_SUCCESS) state = VddState::NotInstalled; // leftover, non-present device node
        else if (problem == CM_PROB_DISABLED) state = VddState::Disabled;
        else if ((status & DN_HAS_PROBLEM) || !(status & DN_STARTED)) state = VddState::Problem;
        else state = VddState::Ready;
        if (detail) *detail = L"status 0x" + std::to_wstring(status) + L", problem " + std::to_wstring(problem);
        if (problemOut) *problemOut = problem;
    }
    SetupDiDestroyDeviceInfoList(set);
    return state;
}

VddState QueryVdd(std::wstring* detail) { return QueryDevice(kDisplayClass, kHardwareId, detail); }

VddState QueryVigem() { return QueryDevice(kSystemClass, kVigemHardwareId, nullptr); }

VddState QueryMic(std::wstring* detail) { return QueryDevice(kMediaClass, kMicHardwareId, detail); }

bool SetupVigem(std::wstring& message) {
    HDEVINFO set = SetupDiGetClassDevsW(&kSystemClass, nullptr, nullptr, 0);
    SP_DEVINFO_DATA d{sizeof(d)};
    ULONG status = 0, problem = 0;
    bool found = set != INVALID_HANDLE_VALUE && FindDevice(set, d, kVigemHardwareId) &&
                 CM_Get_DevNode_Status(&status, &problem, d.DevInst, 0) == CR_SUCCESS;
    if (found && problem == CM_PROB_DISABLED) {
        bool ok = ChangeState(set, d, DICS_ENABLE, DICS_FLAG_GLOBAL) || ChangeState(set, d, DICS_ENABLE, DICS_FLAG_CONFIGSPECIFIC);
        LOGI("vigem: enable device -> %s", ok ? "ok" : Narrow(LastErrorText(GetLastError())).c_str());
    } else if (found && (status & DN_HAS_PROBLEM)) {
        ChangeState(set, d, DICS_PROPCHANGE, DICS_FLAG_GLOBAL); // restart it
    }
    if (set != INVALID_HANDLE_VALUE) SetupDiDestroyDeviceInfoList(set);
    if (!found) {
        message = L"ViGEmBus (game controller driver) is not installed.";
        return false;
    }
    for (int i = 0; i < 15 && QueryVigem() != VddState::Ready; ++i) Sleep(200);
    bool task = CreateToggleTask(kTaskGamepadOn, L"enable", L"game controller driver", kVigemHardwareId, message);
    VddState s = QueryVigem();
    if (task) message = std::wstring(L"ViGEmBus (game controller driver) is ") + VddStateText(s) + L".";
    return task && s == VddState::Ready;
}

namespace {

// ViGEmBus 1.22.0, the project's final release (the repository was archived on 2023-11-02), signed by Nefarius Software Solutions.
const wchar_t kVigemInstaller[] = L"ViGEmBus_1.22.0_x64_x86_arm64.exe";
const char kVigemSha256[] = "89220a7865076b342892f98865f3499fb7c4cfd673159e89d352c360fd014c6a";

} // namespace

bool Sha256Hex(const std::wstring& path, std::string& hex) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE h = nullptr;
    bool ok = BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0 && BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0) == 0;
    std::vector<BYTE> buf(1 << 20);
    DWORD got = 0;
    while (ok && ::ReadFile(f, buf.data(), DWORD(buf.size()), &got, nullptr) && got > 0) ok = BCryptHashData(h, buf.data(), got, 0) == 0;
    BYTE digest[32] = {};
    ok = ok && BCryptFinishHash(h, digest, sizeof(digest), 0) == 0;
    if (h) BCryptDestroyHash(h);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    CloseHandle(f);
    if (!ok) return false;
    static const char kDigits[] = "0123456789abcdef";
    hex.clear();
    for (BYTE b : digest) {
        hex += kDigits[b >> 4];
        hex += kDigits[b & 15];
    }
    return true;
}

bool HasValidSignature(const std::wstring& path) {
    WINTRUST_FILE_INFO file{sizeof(file)};
    file.pcwszFilePath = path.c_str();
    WINTRUST_DATA data{sizeof(data)};
    data.dwUIChoice = WTD_UI_NONE;
    data.fdwRevocationChecks = WTD_REVOKE_NONE; // no network from an administrator process; the pinned hash is the real check
    data.dwUnionChoice = WTD_CHOICE_FILE;
    data.pFile = &file;
    data.dwStateAction = WTD_STATEACTION_VERIFY;
    GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    const LONG r = WinVerifyTrust(HWND(INVALID_HANDLE_VALUE), &action, &data);
    data.dwStateAction = WTD_STATEACTION_CLOSE;
    WinVerifyTrust(HWND(INVALID_HANDLE_VALUE), &action, &data);
    return r == 0;
}

bool SignerCertificate(const std::wstring& signedPath, std::vector<unsigned char>& der, std::wstring& name) {
    // A catalog is a PKCS#7 message and a signed program carries one: read its signer.
    HCERTSTORE store = nullptr;
    HCRYPTMSG msg = nullptr;
    DWORD encoding = 0, content = 0, format = 0;
    if (!CryptQueryObject(CERT_QUERY_OBJECT_FILE, signedPath.c_str(), CERT_QUERY_CONTENT_FLAG_PKCS7_SIGNED | CERT_QUERY_CONTENT_FLAG_PKCS7_SIGNED_EMBED,
                          CERT_QUERY_FORMAT_FLAG_BINARY, 0, &encoding, &content, &format, &store, &msg, nullptr))
        return false;
    bool found = false;
    DWORD size = 0;
    if (CryptMsgGetParam(msg, CMSG_SIGNER_CERT_INFO_PARAM, 0, nullptr, &size) && size) {
        std::vector<BYTE> info(size);
        if (CryptMsgGetParam(msg, CMSG_SIGNER_CERT_INFO_PARAM, 0, info.data(), &size)) {
            PCCERT_CONTEXT cert = CertGetSubjectCertificateFromStore(store, encoding, reinterpret_cast<PCERT_INFO>(info.data()));
            if (cert) {
                wchar_t text[256] = {};
                CertGetNameStringW(cert, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, nullptr, text, 256);
                name = text;
                der.assign(cert->pbCertEncoded, cert->pbCertEncoded + cert->cbCertEncoded);
                found = true;
                CertFreeCertificateContext(cert);
            }
        }
    }
    if (store) CertCloseStore(store, 0);
    if (msg) CryptMsgClose(msg);
    return found;
}

bool SignedByMicrosoftHardware(const std::wstring& catalogPath) {
    std::vector<unsigned char> der;
    std::wstring name;
    return HasValidSignature(catalogPath) && SignerCertificate(catalogPath, der, name) &&
           name.find(L"Microsoft Windows Hardware Compatibility Publisher") != std::wstring::npos;
}

bool TestSigningOn() {
    using Query = LONG(NTAPI*)(ULONG, PVOID, ULONG, PULONG);
    const auto query = reinterpret_cast<Query>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQuerySystemInformation"));
    struct { ULONG length; ULONG options; } info{sizeof(info), 0}; // SYSTEM_CODEINTEGRITY_INFORMATION
    constexpr ULONG kSystemCodeIntegrityInformation = 103, kTestSign = 0x2; // CODEINTEGRITY_OPTION_TESTSIGN
    return query && query(kSystemCodeIntegrityInformation, &info, sizeof(info), nullptr) >= 0 && (info.options & kTestSign) != 0;
}

bool VerifyVigemInstaller(const std::wstring& path, std::wstring& message) {
    std::string hex;
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        message = L"The ViGEmBus installer is missing: " + path;
        return false;
    }
    if (!Sha256Hex(path, hex) || hex != kVigemSha256) {
        message = L"The ViGEmBus installer is not the file PadDisplay shipped, so it was not run.";
        LOGI("vigem: installer hash mismatch (%s)", hex.c_str());
        return false;
    }
    if (!HasValidSignature(path)) {
        message = L"The ViGEmBus installer's signature is not valid, so it was not run.";
        return false;
    }
    return true;
}

bool InstallVigemBus(const std::wstring& driverDir, std::wstring& message) {
    if (QueryVigem() != VddState::NotInstalled) {
        message = L"ViGEmBus (game controller driver) is already installed.";
        return true;
    }
    const std::wstring path = driverDir + L"\\" + kVigemInstaller;
    if (!VerifyVigemInstaller(path, message)) return false;
    // bInheritHandles = FALSE: the installer must not inherit any handle of this process (see CLAUDE.md).
    std::wstring cmd = L"\"" + path + L"\" /exenoui /qn /norestart";
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, driverDir.c_str(), &si, &pi)) {
        message = L"Could not start the ViGEmBus installer: " + LastErrorText(GetLastError());
        return false;
    }
    CloseHandle(pi.hThread);
    DWORD wait = WaitForSingleObject(pi.hProcess, 180000);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    if (wait != WAIT_OBJECT_0) {
        message = L"The ViGEmBus installer did not finish in time.";
        return false;
    }
    if (code != 0 && code != 3010) { // 3010: installed, a restart is needed
        message = L"The ViGEmBus installer failed (exit code " + std::to_wstring(code) + L").";
        return false;
    }
    MarkAddedByPadDisplay(kAddedVigem, true); // it was not on this PC before: the uninstaller may take it away again
    for (int i = 0; i < 50 && QueryVigem() != VddState::Ready; ++i) Sleep(200);
    VddState s = QueryVigem();
    message = s == VddState::Ready ? (code == 3010 ? L"Game controller driver installed. Restart Windows to finish."
                                                   : L"Game controller driver installed.")
                                   : std::wstring(L"ViGEmBus was installed, but is ") + VddStateText(s) + L".";
    LOGI("vigem: %s", Narrow(message).c_str());
    return s == VddState::Ready;
}

namespace {
// ViGEmBus registers as a Windows Installer product. Returns its product code ("{...}"), or nothing if it is not installed that way.
std::wstring VigemProductCode() {
    for (REGSAM view : {KEY_WOW64_64KEY, KEY_WOW64_32KEY}) {
        HKEY products = nullptr;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall", 0, KEY_READ | view, &products) != ERROR_SUCCESS) continue;
        std::wstring code;
        wchar_t product[256];
        for (DWORD i = 0, length = 256; code.empty() && RegEnumKeyExW(products, i, product, &length, nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
             ++i, length = 256) {
            wchar_t name[256] = {};
            DWORD size = sizeof(name);
            if (product[0] == L'{' && RegGetValueW(products, product, L"DisplayName", RRF_RT_REG_SZ, nullptr, name, &size) == ERROR_SUCCESS &&
                wcscmp(name, L"ViGEm Bus Driver") == 0)
                code = product;
        }
        RegCloseKey(products);
        if (!code.empty()) return code;
    }
    return {};
}
} // namespace

bool RemoveVigem(std::wstring& message) {
    const std::wstring product = VigemProductCode();
    if (product.empty()) {
        message = L"ViGEmBus is not installed as a program, so there was nothing to remove.";
        MarkAddedByPadDisplay(kAddedVigem, false);
        return true;
    }
    DWORD code = 1;
    RunCapture(L"\"" + SystemProgram(L"msiexec.exe") + L"\" /x " + product + L" /qn /norestart", 180000, &code);
    const bool removed = code == 0 || code == 3010; // 3010: removed, a restart finishes it
    message = !removed ? L"Windows Installer could not remove ViGEmBus (exit code " + std::to_wstring(code) + L")."
                       : code == 3010 ? L"Game controller driver removed. Restart Windows to finish." : L"Game controller driver removed.";
    LOGI("vigem: %s", Narrow(message).c_str());
    if (removed) MarkAddedByPadDisplay(kAddedVigem, false);
    return removed;
}

bool EnableVigem() {
    DWORD code = 1;
    RunCapture(L"schtasks.exe /run /tn \"" + std::wstring(kTaskGamepadOn) + L"\"", 10000, &code);
    LOGI("vigem: switch-on task %s", code == 0 ? "started" : "is not registered (run the admin setup from the tray)");
    return code == 0;
}

namespace {
// The files the 64-bit Windows 10/11 install uses must be exactly the shipped ones before VB-Audio's installer runs.
bool VerifyCablePackage(const std::wstring& driverDir, std::wstring& message) {
    static const struct { const wchar_t* file; const char* sha256; bool signedFile; } kFiles[] = {
        {L"VBCABLE_Setup_x64.exe", "734c35dfa6d98f48782a451633ceb471166ec70d60482fd89a1123d0ee3c4f41", true},
        {L"vbaudio_cable64_win10.sys", "f01344602472f1b527de5ee98f18987c03db48fea444e457d061e937f1d531d5", false},
        {L"vbaudio_cable64_win10.cat", "0a921ebadbe39cf3fa7c14ff37d1ca22565a9651244bd3ac177dc810bc99072e", false},
        {L"vbMmeCable64_win10.inf", "61c857be74831cc299d9be62f8d49d137f14063454fc54f859d5bc9b4b813daf", false},
    };
    for (const auto& f : kFiles) {
        const std::wstring path = driverDir + L"\\" + f.file;
        std::string hex;
        if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
            message = L"VB-CABLE is missing from this PadDisplay: " + path;
            return false;
        }
        if (!Sha256Hex(path, hex) || hex != f.sha256) {
            message = std::wstring(L"The VB-CABLE file ") + f.file + L" is not the one PadDisplay shipped, so the installer was not run.";
            LOGI("cable: hash mismatch for %ls (%s)", f.file, hex.c_str());
            return false;
        }
        if (f.signedFile && !HasValidSignature(path)) {
            message = std::wstring(L"The signature of ") + f.file + L" is not valid, so the installer was not run.";
            return false;
        }
    }
    return true;
}

// Runs VB-Audio's installer with its switches for unattended use: -i installs, -u uninstalls, -h hides its window.
// bInheritHandles = FALSE: the installer must not inherit any handle of this process (see CLAUDE.md).
bool RunCableSetup(const std::wstring& driverDir, const wchar_t* switches, std::wstring& message) {
    std::wstring cmd = L"\"" + driverDir + L"\\VBCABLE_Setup_x64.exe\" " + switches;
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, driverDir.c_str(), &si, &pi)) {
        message = L"Could not start the VB-CABLE installer: " + LastErrorText(GetLastError());
        return false;
    }
    CloseHandle(pi.hThread);
    const DWORD wait = WaitForSingleObject(pi.hProcess, 180000);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    LOGI("cable: installer %ls finished (wait %lu, exit code %lu)", switches, wait, code);
    if (wait != WAIT_OBJECT_0) message = L"The VB-CABLE installer did not finish in time.";
    return wait == WAIT_OBJECT_0;
}
} // namespace

// VB-CABLE Pack45 (VB-Audio Software, donationware), unmodified. Its driver is signed by Microsoft's hardware publisher (WHQL),
// so Windows installs it without asking about the publisher.
bool InstallCable(const std::wstring& driverDir, std::wstring& message) {
    std::wstring name;
    if (CableInstalled(L"", &name)) {
        message = L"VB-CABLE is already installed (" + name + L").";
        return true;
    }
    if (!VerifyCablePackage(driverDir, message) || !RunCableSetup(driverDir, L"-i -h", message)) return false;
    MarkAddedByPadDisplay(kAddedCable, true); // it was not on this PC before: the uninstaller may take it away again
    for (int i = 0; i < 150 && !CableInstalled(L"", &name); ++i) Sleep(200); // the device appears a moment after the installer ends
    const bool present = CableInstalled(L"", &name);
    message = present ? L"VB-CABLE installed. In your apps choose \"CABLE Output\" as the microphone."
                      : L"VB-CABLE was installed, but its device has not appeared yet. Restart Windows to finish (VB-Audio's note).";
    LOGI("cable: %s", Narrow(message).c_str());
    return present;
}

bool RemoveCable(const std::wstring& driverDir, std::wstring& message) {
    if (!CableInstalled(L"")) {
        message = L"VB-CABLE is not installed.";
        MarkAddedByPadDisplay(kAddedCable, false);
        return true;
    }
    if (!VerifyCablePackage(driverDir, message) || !RunCableSetup(driverDir, L"-u -h", message)) return false;
    for (int i = 0; i < 100 && CableInstalled(L""); ++i) Sleep(200); // the device goes a moment after the installer ends
    const bool gone = !CableInstalled(L"");
    message = gone ? L"VB-CABLE removed." : L"VB-CABLE was uninstalled, but its device is still there. Restart Windows to finish.";
    LOGI("cable: %s", Narrow(message).c_str());
    MarkAddedByPadDisplay(kAddedCable, false);
    return gone;
}

bool InstallMicDriver(const std::wstring& driverDir, std::wstring& message) {
    const std::wstring inf = driverDir + L"\\padmic.inf";
    if (GetFileAttributesW(inf.c_str()) == INVALID_FILE_ATTRIBUTES) {
        message = L"Microphone driver package not found: " + inf;
        return false;
    }
    bool created = false;
    HDEVINFO set = SetupDiGetClassDevsW(&kMediaClass, nullptr, nullptr, 0); // includes devices that are not present
    SP_DEVINFO_DATA d{sizeof(d)};
    if (set != INVALID_HANDLE_VALUE && FindDevice(set, d, kMicHardwareId)) {
        ULONG status = 0, problem = 0;
        if (CM_Get_DevNode_Status(&status, &problem, d.DevInst, 0) == CR_SUCCESS && problem == CM_PROB_DISABLED)
            (void)(ChangeState(set, d, DICS_ENABLE, DICS_FLAG_GLOBAL) || ChangeState(set, d, DICS_ENABLE, DICS_FLAG_CONFIGSPECIFIC));
        SetupDiDestroyDeviceInfoList(set);
    } else {
        if (set != INVALID_HANDLE_VALUE) SetupDiDestroyDeviceInfoList(set);
        // Create the root-enumerated device (like "devcon install"), then bind the driver to it.
        set = SetupDiCreateDeviceInfoList(&kMediaClass, nullptr);
        if (set == INVALID_HANDLE_VALUE) {
            message = L"SetupDiCreateDeviceInfoList: " + LastErrorText(GetLastError());
            return false;
        }
        d = {sizeof(d)};
        const wchar_t hwid[] = L"Root\\PadDisplayMic\0"; // REG_MULTI_SZ: the literal's own NUL plus this one
        created = SetupDiCreateDeviceInfoW(set, L"MEDIA", &kMediaClass, L"PadDisplay Virtual Microphone", nullptr, DICD_GENERATE_ID, &d) &&
                  SetupDiSetDeviceRegistryPropertyW(set, &d, SPDRP_HARDWAREID, reinterpret_cast<const BYTE*>(hwid), sizeof(hwid)) &&
                  SetupDiCallClassInstaller(DIF_REGISTERDEVICE, set, &d);
        if (!created) {
            message = L"Could not create the microphone device: " + LastErrorText(GetLastError());
            SetupDiDestroyDeviceInfoList(set);
            return false;
        }
    }
    BOOL reboot = FALSE;
    if (!UpdateDriverForPlugAndPlayDevicesW(nullptr, kMicHardwareId, inf.c_str(), INSTALLFLAG_FORCE, &reboot)) {
        const DWORD err = GetLastError();
        if (created) SetupDiCallClassInstaller(DIF_REMOVE, set, &d); // leave no half-installed device behind
        if (set != INVALID_HANDLE_VALUE) SetupDiDestroyDeviceInfoList(set);
        message = L"Microphone driver install failed: " + LastErrorText(err) +
                  L". A kernel driver must be signed by Microsoft when Secure Boot is on; if this build is not, Windows refuses it.";
        LOGI("mic driver: %s", Narrow(message).c_str());
        return false;
    }
    if (set != INVALID_HANDLE_VALUE) SetupDiDestroyDeviceInfoList(set);
    VddState s = VddState::NotInstalled;
    ULONG problem = 0;
    std::wstring detail;
    for (int i = 0; i < 30; ++i) {
        s = QueryDevice(kMediaClass, kMicHardwareId, &detail, &problem);
        if (s == VddState::Ready) break;
        Sleep(200);
    }
    if (s == VddState::Ready) {
        message = reboot ? L"Microphone driver installed. Restart Windows to finish." : L"Microphone driver installed.";
        // The driver is only loaded while the tray's microphone option is on: these tasks let PadDisplay switch the device on
        // and off without another administrator prompt (they can do nothing else, see CreateToggleTask).
        std::wstring taskMessage;
        if (!CreateToggleTask(kTaskMicOff, L"disable", L"PadDisplay microphone", kMicHardwareId, taskMessage) ||
            !CreateToggleTask(kTaskMicOn, L"enable", L"PadDisplay microphone", kMicHardwareId, taskMessage))
            message += L" (Switching it on and off from the tray will not work: " + taskMessage + L")";
    }
    else if (problem == CM_PROB_UNSIGNED_DRIVER)
        message = L"Windows will not load the microphone driver: it is not signed by Microsoft (problem " + std::to_wstring(problem) + L").";
    else message = std::wstring(L"The microphone driver was installed, but is ") + VddStateText(s) + L" (" + detail + L").";
    LOGI("mic driver: %s", Narrow(message).c_str());
    return s == VddState::Ready;
}

int MicRecorders() {
    HANDLE dev = CreateFileW(PADMIC_USER_PATH, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (dev == INVALID_HANDLE_VALUE) {
        const DWORD err = GetLastError();
        return (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND) ? 0 : -1;
    }
    PADMIC_STATUS st{};
    DWORD got = 0;
    const bool ok = DeviceIoControl(dev, IOCTL_PADMIC_GET_STATUS, nullptr, 0, &st, sizeof(st), &got, nullptr) && got >= PADMIC_STATUS_V1_SIZE;
    CloseHandle(dev);
    return ok ? int(st.StreamsRunning) : -1;
}

bool MicRestartPending() {
    HDEVINFO set = SetupDiGetClassDevsW(&kMediaClass, nullptr, nullptr, 0);
    if (set == INVALID_HANDLE_VALUE) return false;
    SP_DEVINFO_DATA d{sizeof(d)};
    bool pending = false;
    if (FindDevice(set, d, kMicHardwareId)) {
        ULONG status = 0, problem = 0;
        pending = CM_Get_DevNode_Status(&status, &problem, d.DevInst, 0) == CR_SUCCESS && (status & DN_NEED_RESTART) != 0;
    }
    SetupDiDestroyDeviceInfoList(set);
    return pending;
}

bool SetMicDriverEnabled(bool on, unsigned timeoutMs) {
    const VddState want = on ? VddState::Ready : VddState::Disabled;
    const VddState now = QueryMic();
    if (now == want) return true;
    if (now == VddState::NotInstalled) return false;
    DWORD code = 1;
    RunCapture(L"schtasks.exe /run /tn \"" + std::wstring(on ? kTaskMicOn : kTaskMicOff) + L"\"", 10000, &code);
    if (code != 0) {
        LOGI("mic driver: cannot start the %s task (%lu): run the repair from the tray", on ? "on" : "off", code);
        return false;
    }
    const ULONGLONG deadline = GetTickCount64() + timeoutMs;
    while (GetTickCount64() < deadline) {
        if (QueryMic() == want) {
            LOGI("mic driver: %s", on ? "enabled (loaded)" : "disabled (not loaded)");
            return true;
        }
        Sleep(100);
    }
    LOGI("mic driver: did not %s in time%s", on ? "come up" : "go away",
         MicRestartPending() ? " (Windows is waiting for a restart to finish an earlier change to this device)" : "");
    return false;
}

bool RemoveMicDriver(std::wstring& message) {
    if (MicRecorders() > 0) { // see MicRecorders: removing the device under a recording leaves it stuck until a restart
        message = L"An app is recording from the PadDisplay microphone right now. Close it (or switch its microphone to another device) and try again.";
        LOGI("mic driver: %s", Narrow(message).c_str());
        return false;
    }
    const bool removedDevice = RemoveDevice(kMediaClass, kMicHardwareId);
    DeleteTask(kTaskMicOn);
    DeleteTask(kTaskMicOff);
    const int packages = DeleteDriverPackages("root\\paddisplaymic");
    message = removedDevice || packages ? L"Microphone driver removed." : L"The microphone driver was not installed.";
    LOGI("mic driver: %s (device %s, %d package(s))", Narrow(message).c_str(), removedDevice ? "removed" : "none", packages);
    return true;
}

bool InstallVdd(const std::wstring& driverDir, std::wstring& message) {
    if (!InstallDriver(driverDir, message)) return false;
    std::wstring taskMessage;
    if (!InstallToggleTasks(taskMessage)) message += L" (Removing it on exit will not work: " + taskMessage + L")";
    return true;
}

void RemoveToggleTasks() {
    for (const wchar_t* task : {kTaskOn, kTaskOff, kTaskGamepadOn, kTaskMicOn, kTaskMicOff}) DeleteTask(task);
}

bool RemoveVdd(std::wstring& message) {
    const bool removedDevice = RemoveDevice(kDisplayClass, kHardwareId);
    const int packages = DeleteDriverPackages("root\\mttvdd");
    DeleteSettingsFile();
    const bool gone = QueryVdd() == VddState::NotInstalled;
    message = !gone ? L"The virtual monitor driver could not be removed." : removedDevice || packages ? L"Virtual monitor driver removed."
                                                                                                    : L"The virtual monitor driver was not installed.";
    LOGI("vdd: %s (device %s, %d package(s))", Narrow(message).c_str(), removedDevice ? "removed" : "none", packages);
    if (gone) MarkAddedByPadDisplay(kAddedMonitor, false);
    return gone;
}

namespace {
bool InstallDriver(const std::wstring& driverDir, std::wstring& message) {
    std::wstring inf = driverDir + L"\\MttVDD.inf";
    if (GetFileAttributesW(inf.c_str()) == INVALID_FILE_ATTRIBUTES) {
        message = L"Driver package not found: " + inf;
        return false;
    }
    bool wroteSettings = false;
    if (!EnsureSettingsFile(&wroteSettings)) {
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
        if (wroteSettings) DeleteSettingsFile();
        return false;
    }
    BOOL reboot = FALSE;
    if (!UpdateDriverForPlugAndPlayDevicesW(nullptr, kHardwareId, inf.c_str(), INSTALLFLAG_FORCE, &reboot)) {
        DWORD err = GetLastError();
        SetupDiCallClassInstaller(DIF_REMOVE, set, &d);
        SetupDiDestroyDeviceInfoList(set);
        message = L"Driver install failed: " + InstallErrorText(err);
        LOGI("vdd: %s", Narrow(message).c_str());
        if (wroteSettings) DeleteSettingsFile(); // nothing of a failed install stays behind
        return false;
    }
    SetupDiDestroyDeviceInfoList(set);
    for (int i = 0; i < 30 && QueryVdd() != VddState::Ready; ++i) Sleep(200);
    VddState s = QueryVdd();
    message = s == VddState::Ready ? (reboot ? L"Virtual monitor driver installed. Restart Windows to finish."
                                             : L"Virtual monitor driver installed.")
                                   : std::wstring(L"Installed, but the driver is ") + VddStateText(s);
    LOGI("vdd: %s", Narrow(message).c_str());
    MarkAddedByPadDisplay(kAddedMonitor, true); // the device did not exist before: the uninstaller may take it away again
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
