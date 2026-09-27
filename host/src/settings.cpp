// PadDisplay by Nezucloud. Copyright (c) 2026 Nezucloud.
// SPDX-License-Identifier: MIT
#include "settings.h"

#include <windows.h>
#include <shlobj.h>
#include <algorithm>
#include <random>

namespace pd {

std::wstring AppDataDir() {
    PWSTR base = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &base))) dir = base;
    CoTaskMemFree(base);
    dir += L"\\PadDisplay";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

static std::wstring IniPath() { return AppDataDir() + L"\\settings.ini"; }

static int GetInt(const wchar_t* key, int def) {
    return int(GetPrivateProfileIntW(L"host", key, def, IniPath().c_str()));
}

static std::wstring GetStr(const wchar_t* key, const std::wstring& def) {
    wchar_t buf[512];
    GetPrivateProfileStringW(L"host", key, def.c_str(), buf, 512, IniPath().c_str());
    return buf;
}

static void PutStr(const wchar_t* key, const std::wstring& v) {
    WritePrivateProfileStringW(L"host", key, v.c_str(), IniPath().c_str());
}

static void PutInt(const wchar_t* key, long long v) { PutStr(key, std::to_wstring(v)); }

void Settings::Load() {
    usbBitrateMbps = GetInt(L"usbBitrateMbps", usbBitrateMbps);
    wifiBitrateMbps = GetInt(L"wifiBitrateMbps", wifiBitrateMbps);
    minBitrateMbps = GetInt(L"minBitrateMbps", minBitrateMbps);
    adaptive = GetInt(L"adaptive", adaptive) != 0;
    wifiQos = GetInt(L"wifiQos", wifiQos) != 0;
    maxFramesInFlight = std::clamp(GetInt(L"maxFramesInFlight", maxFramesInFlight), 1, 10);
    codec = GetStr(L"codec", codec);
    maxFps = std::clamp(GetInt(L"maxFps", maxFps), 10, 240);
    encoder = GetStr(L"encoder", encoder);
    nvencPreset = std::clamp(GetInt(L"nvencPreset", nvencPreset), 0, 7);
    drawCursor = GetInt(L"drawCursor", drawCursor) != 0;
    matchClientResolution = GetInt(L"matchClientResolution", matchClientResolution) != 0;
    detachOnDisconnect = GetInt(L"detachOnDisconnect", detachOnDisconnect) != 0;
    removeMonitorOnExit = GetInt(L"removeMonitorOnExit", removeMonitorOnExit) != 0;
    gpuMaxPerformance = GetInt(L"gpuMaxPerformance", gpuMaxPerformance) != 0;
    profile = GetInt(L"profile", profile) != 0;
    displayOverride = GetStr(L"display", L"");
    adbPath = GetStr(L"adbPath", L"");
    requireUsbPin = GetInt(L"requireUsbPin", requireUsbPin) != 0;
    pin = uint32_t(wcstoul(GetStr(L"pin", L"0").c_str(), nullptr, 10));
    if (pin < 100000 || pin > 999999) {
        std::random_device rd;
        pin = 100000 + rd() % 900000;
    }
    Save();
}

void Settings::Save() const {
    PutInt(L"usbBitrateMbps", usbBitrateMbps);
    PutInt(L"wifiBitrateMbps", wifiBitrateMbps);
    PutInt(L"minBitrateMbps", minBitrateMbps);
    PutInt(L"adaptive", adaptive);
    PutInt(L"maxFramesInFlight", maxFramesInFlight);
    PutInt(L"wifiQos", wifiQos);
    PutStr(L"codec", codec);
    PutInt(L"maxFps", maxFps);
    PutStr(L"encoder", encoder);
    PutInt(L"nvencPreset", nvencPreset);
    PutInt(L"drawCursor", drawCursor);
    PutInt(L"matchClientResolution", matchClientResolution);
    PutInt(L"detachOnDisconnect", detachOnDisconnect);
    PutInt(L"removeMonitorOnExit", removeMonitorOnExit);
    PutInt(L"gpuMaxPerformance", gpuMaxPerformance);
    PutStr(L"display", displayOverride);
    PutStr(L"adbPath", adbPath);
    PutInt(L"requireUsbPin", requireUsbPin);
    PutInt(L"pin", pin);
}

} // namespace pd
