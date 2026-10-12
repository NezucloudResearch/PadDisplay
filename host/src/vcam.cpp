// PadDisplay by Nezucloud. Copyright (c) 2026 Nezucloud.
// SPDX-License-Identifier: MIT
#include "vcam.h"
// The virtual camera declarations are hidden below Windows 11 by the SDK headers. This file only uses them through
// function pointers found at run time, so the exe still starts on Windows 10.
#undef NTDDI_VERSION
#define NTDDI_VERSION 0x0A00000B

#include "log.h"

#include <windows.h>
#include <knownfolders.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfvirtualcamera.h>
#include <shlobj.h>
#include <wrl/client.h>

namespace pd {
namespace {

// Must match CLSID_PadDisplayCamera in camera/camera.cpp.
const GUID kCameraClsid = {0xa3c1d2f4, 0x5b6e, 0x4f70, {0x8a, 0x91, 0x6d, 0x2e, 0x0b, 0x7c, 0x4e, 0x15}};
const wchar_t kClsidText[] = L"{A3C1D2F4-5B6E-4F70-8A91-6D2E0B7C4E15}";
const wchar_t kDllName[] = L"PadDisplayCamera.dll";
const wchar_t kFriendlyName[] = L"PadDisplay Camera";
const wchar_t kMarkerKey[] = L"SOFTWARE\\PadDisplay";
const wchar_t kMarkerValue[] = L"VirtualCamera";

using CreateVirtualCameraFn = HRESULT(WINAPI*)(MFVirtualCameraType, MFVirtualCameraLifetime, MFVirtualCameraAccess, LPCWSTR, LPCWSTR,
                                               const GUID*, ULONG, IMFVirtualCamera**);

bool OsHasVirtualCamera() {
    using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
    auto fn = reinterpret_cast<RtlGetVersionFn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"));
    OSVERSIONINFOW v{sizeof(v)};
    return fn && fn(&v) == 0 && v.dwBuildNumber >= 22000;
}

std::wstring InstallDir() {
    PWSTR base = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_ProgramFiles, 0, nullptr, &base))) dir = base;
    CoTaskMemFree(base);
    return dir + L"\\PadDisplay\\Camera";
}

bool MarkerSet() {
    DWORD v = 0, size = sizeof(v);
    return RegGetValueW(HKEY_LOCAL_MACHINE, kMarkerKey, kMarkerValue, RRF_RT_REG_DWORD | RRF_SUBKEY_WOW6464KEY, nullptr, &v, &size) == ERROR_SUCCESS && v == 1;
}

void SetMarker(bool on) {
    HKEY key = nullptr;
    if (on) {
        if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, kMarkerKey, 0, nullptr, 0, KEY_WRITE | KEY_WOW64_64KEY, nullptr, &key, nullptr) != ERROR_SUCCESS) return;
        DWORD v = 1;
        RegSetValueExW(key, kMarkerValue, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&v), sizeof(v));
        RegCloseKey(key);
    } else if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kMarkerKey, 0, KEY_WRITE | KEY_WOW64_64KEY, &key) == ERROR_SUCCESS) {
        RegDeleteValueW(key, kMarkerValue);
        RegCloseKey(key);
    }
}

// The virtual camera is keyed by these parameters: the same call later opens (and so can Remove) the same camera.
HRESULT OpenVirtualCamera(Microsoft::WRL::ComPtr<IMFVirtualCamera>& vcam) {
    HMODULE lib = LoadLibraryW(L"mfsensorgroup.dll");
    auto create = lib ? reinterpret_cast<CreateVirtualCameraFn>(GetProcAddress(lib, "MFCreateVirtualCamera")) : nullptr;
    if (!create) return HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
    return create(MFVirtualCameraType_SoftwareCameraSource, MFVirtualCameraLifetime_System, MFVirtualCameraAccess_AllUsers, kFriendlyName,
                  kClsidText, nullptr, 0, &vcam);
}

std::wstring HrText(HRESULT hr) {
    wchar_t buf[512] = {};
    FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, DWORD(hr), 0, buf, 512, nullptr);
    std::wstring s = buf;
    while (!s.empty() && (s.back() == L'\r' || s.back() == L'\n' || s.back() == L' ')) s.pop_back();
    wchar_t code[16];
    swprintf_s(code, L" (0x%08lX)", static_cast<unsigned long>(hr));
    return s + code;
}

// A DLL that the Frame Server still has loaded can be neither deleted nor overwritten, but it can be renamed: this moves it
// out of the way under a name nothing else uses and has Windows delete it at the next restart.
void SetAsideUntilRestart(const std::wstring& path) {
    const std::wstring aside = path + L"." + std::to_wstring(GetTickCount64()) + L".old";
    if (MoveFileExW(path.c_str(), aside.c_str(), 0)) MoveFileExW(aside.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
}

} // namespace

CamState QueryCamera() {
    if (!OsHasVirtualCamera()) return CamState::Unsupported;
    if (!MarkerSet()) return CamState::NotInstalled;
    if (GetFileAttributesW((InstallDir() + L"\\" + kDllName).c_str()) == INVALID_FILE_ATTRIBUTES) return CamState::NotInstalled;
    return CamState::Ready;
}

const wchar_t* CamStateText(CamState s) {
    switch (s) {
    case CamState::Unsupported: return L"needs Windows 11";
    case CamState::NotInstalled: return L"not set up";
    case CamState::Ready: return L"ready";
    }
    return L"?";
}

bool InstallCamera(const std::wstring& packageDir, std::wstring& message) {
    if (!OsHasVirtualCamera()) {
        message = L"The virtual camera needs Windows 11.";
        return false;
    }
    const std::wstring source = packageDir + L"\\" + kDllName;
    if (GetFileAttributesW(source.c_str()) == INVALID_FILE_ATTRIBUTES) {
        message = L"Camera package not found: " + source;
        return false;
    }
    const std::wstring dir = InstallDir();
    SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
    const std::wstring target = dir + L"\\" + kDllName;
    bool copied = CopyFileW(source.c_str(), target.c_str(), FALSE) != FALSE;
    if (!copied && GetFileAttributesW(target.c_str()) != INVALID_FILE_ATTRIBUTES) { // an upgrade while the camera is in use
        SetAsideUntilRestart(target);
        copied = CopyFileW(source.c_str(), target.c_str(), FALSE) != FALSE;
    }
    if (!copied) {
        message = L"Could not copy the camera to " + dir + L" (error " + std::to_wstring(GetLastError()) + L").";
        return false;
    }

    // Register the COM class from the installed copy, in this process (no regsvr32 child process).
    HMODULE dll = LoadLibraryW(target.c_str());
    auto registerServer = dll ? reinterpret_cast<HRESULT(STDAPICALLTYPE*)()>(GetProcAddress(dll, "DllRegisterServer")) : nullptr;
    HRESULT hr = registerServer ? registerServer() : HRESULT_FROM_WIN32(GetLastError());
    if (dll) FreeLibrary(dll);
    if (FAILED(hr)) {
        message = L"Could not register the camera: " + HrText(hr);
        return false;
    }

    const bool initCom = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
    MFStartup(MF_VERSION, MFSTARTUP_LITE);
    Microsoft::WRL::ComPtr<IMFVirtualCamera> vcam;
    hr = OpenVirtualCamera(vcam);
    if (SUCCEEDED(hr)) hr = vcam->Start(nullptr);
    if (vcam) vcam.Reset();
    MFShutdown();
    if (initCom) CoUninitialize();
    if (FAILED(hr)) {
        message = L"Could not create the virtual camera: " + HrText(hr);
        LOGI("camera: %s", Narrow(message).c_str());
        return false;
    }
    SetMarker(true);
    message = L"Virtual camera installed.";
    LOGI("camera: %s", Narrow(message).c_str());
    return true;
}

bool RemoveCamera(std::wstring& message) {
    const bool initCom = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
    MFStartup(MF_VERSION, MFSTARTUP_LITE);
    Microsoft::WRL::ComPtr<IMFVirtualCamera> vcam;
    HRESULT hr = OpenVirtualCamera(vcam);
    if (SUCCEEDED(hr)) {
        hr = vcam->Remove();
        vcam->Shutdown();
    }
    vcam.Reset();
    MFShutdown();
    if (initCom) CoUninitialize();

    const std::wstring target = InstallDir() + L"\\" + kDllName;
    HMODULE dll = LoadLibraryW(target.c_str());
    if (auto unregisterServer = dll ? reinterpret_cast<HRESULT(STDAPICALLTYPE*)()>(GetProcAddress(dll, "DllUnregisterServer")) : nullptr)
        unregisterServer();
    if (dll) FreeLibrary(dll);
    // The Frame Server keeps the DLL loaded for a while after its camera is gone (more than 10 s, measured). Rather than
    // wait for it, leave the rest to Windows: the file and the then empty folders go at the next restart.
    const std::wstring dir = InstallDir();
    if (DeleteFileW(target.c_str()) || GetLastError() == ERROR_FILE_NOT_FOUND) {
        RemoveDirectoryW(dir.c_str());
    } else {
        SetAsideUntilRestart(target);
        MoveFileExW(dir.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
        MoveFileExW(dir.substr(0, dir.find_last_of(L'\\')).c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT); // only if PadDisplay is gone too
        LOGI("camera: %ls is still loaded by the camera service; Windows deletes it at the next restart", target.c_str());
    }
    SetMarker(false);
    message = SUCCEEDED(hr) ? L"Virtual camera removed." : L"Virtual camera removed, except: " + HrText(hr);
    LOGI("camera: %s", Narrow(message).c_str());
    return SUCCEEDED(hr);
}

} // namespace pd
