// PadDisplay by Nezucloud. Copyright (c) 2026 Nezucloud.
// SPDX-License-Identifier: MIT
#include "settings.h"

#include <windows.h>
#include <shlobj.h>
#include <algorithm>
#include <random>

namespace pd
{
    namespace
    {

        constexpr wchar_t kSection[] = L"host";
        constexpr uint32_t kPinMin = 100000, kPinMax = 999999;

        class IniReader
        {
        public:
            explicit IniReader(const std::wstring &path) : path_(path) {}
            void Int(const wchar_t *key, int &value) const { value = int(GetPrivateProfileIntW(kSection, key, value, path_.c_str())); }
            void Clamped(const wchar_t *key, int &value, int lowest, int highest) const
            {
                Int(key, value);
                value = std::clamp(value, lowest, highest);
            }
            void Bool(const wchar_t *key, bool &value) const { value = GetPrivateProfileIntW(kSection, key, value, path_.c_str()) != 0; }
            void Text(const wchar_t *key, std::wstring &value) const
            {
                wchar_t buffer[512];
                GetPrivateProfileStringW(kSection, key, value.c_str(), buffer, 512, path_.c_str());
                value = buffer;
            }
            void Number(const wchar_t *key, uint32_t &value) const
            {
                std::wstring text = L"0";
                Text(key, text);
                value = uint32_t(wcstoul(text.c_str(), nullptr, 10));
            }

        private:
            const std::wstring &path_;
        };

        class IniWriter
        {
        public:
            explicit IniWriter(const std::wstring &path) : path_(path) {}
            void Int(const wchar_t *key, int value) const { Text(key, std::to_wstring(value)); }
            void Clamped(const wchar_t *key, int value, int, int) const { Int(key, value); }
            void Bool(const wchar_t *key, bool value) const { Int(key, value); }
            void Text(const wchar_t *key, const std::wstring &value) const { WritePrivateProfileStringW(kSection, key, value.c_str(), path_.c_str()); }
            void Number(const wchar_t *key, uint32_t value) const { Text(key, std::to_wstring(value)); }

        private:
            const std::wstring &path_;
        };

        // The one list of what settings.ini holds. Reading and writing both walk it, so a key cannot end up in only one of them.
        template <class Ini, class AnySettings>
        void ForEachSetting(const Ini &ini, AnySettings &s)
        {
            ini.Int(L"usbBitrateMbps", s.usbBitrateMbps);
            ini.Int(L"wifiBitrateMbps", s.wifiBitrateMbps);
            ini.Int(L"minBitrateMbps", s.minBitrateMbps);
            ini.Bool(L"adaptive", s.adaptive);
            ini.Clamped(L"maxFramesInFlight", s.maxFramesInFlight, 1, 10);
            ini.Bool(L"wifiQos", s.wifiQos);
            ini.Text(L"codec", s.codec);
            ini.Clamped(L"maxFps", s.maxFps, 10, 240);
            ini.Text(L"encoder", s.encoder);
            ini.Text(L"gpu", s.gpu);
            ini.Clamped(L"nvencPreset", s.nvencPreset, 0, 7);
            ini.Bool(L"gamingMode", s.gamingMode);
            ini.Bool(L"drawCursor", s.drawCursor);
            ini.Bool(L"matchClientResolution", s.matchClientResolution);
            ini.Bool(L"monitorOnlyWhileConnected", s.monitorOnlyWhileConnected);
            ini.Bool(L"removeMonitorOnExit", s.removeMonitorOnExit);
            ini.Clamped(L"reconnectGraceSeconds", s.reconnectGraceSeconds, 0, 600);
            ini.Bool(L"gpuMaxPerformance", s.gpuMaxPerformance);
            ini.Text(L"display", s.displayOverride);
            ini.Text(L"adbPath", s.adbPath);
            ini.Bool(L"requireUsbPin", s.requireUsbPin);
            ini.Bool(L"requireEncryption", s.requireEncryption);
            ini.Bool(L"acceptTabletMic", s.acceptTabletMic);
            ini.Text(L"micBackend", s.micBackend);
            ini.Text(L"micDevice", s.micDevice);
            ini.Bool(L"cableMinPeriod", s.cableMinPeriod);
            ini.Bool(L"acceptTabletCamera", s.acceptTabletCamera);
            ini.Bool(L"uacClickableOnTablet", s.uacClickableOnTablet);
            ini.Bool(L"lockScreenControl", s.lockScreenControl);
            ini.Bool(L"preSignInAccess", s.preSignInAccess);
            ini.Bool(L"runAsAdmin", s.runAsAdmin);
            ini.Bool(L"autostartTask", s.autostartTask);
            ini.Number(L"pin", s.pin);
        }

        uint32_t NewPin()
        {
            std::random_device random;
            return kPinMin + random() % (kPinMax - kPinMin + 1);
        }

        std::wstring IniPath() { return AppDataDir() + L"\\settings.ini"; }

    } // namespace

    std::wstring AppDataDir()
    {
        PWSTR base = nullptr;
        std::wstring dir;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &base)))
            dir = base;
        CoTaskMemFree(base);
        dir += L"\\PadDisplay";
        CreateDirectoryW(dir.c_str(), nullptr);
        return dir;
    }

    std::wstring MachineDataDir()
    {
        wchar_t programData[MAX_PATH] = {};
        const DWORD n = GetEnvironmentVariableW(L"ProgramData", programData, MAX_PATH);
        std::wstring dir = (n > 0 && n < MAX_PATH) ? std::wstring(programData) : std::wstring(L"C:\\ProgramData");
        dir += L"\\PadDisplay";
        CreateDirectoryW(dir.c_str(), nullptr);
        return dir;
    }

    void Settings::Load() { LoadFrom(IniPath()); }
    void Settings::Save() const { SaveTo(IniPath()); }

    void Settings::LoadFrom(const std::wstring &iniPath)
    {
        const IniReader ini(iniPath);
        ForEachSetting(ini, *this);
        ini.Bool(L"profile", profile); // a diagnostic switch: it stays out of the file until someone adds it by hand
        if (pin < kPinMin || pin > kPinMax)
            pin = NewPin();
        SaveTo(iniPath);
    }

    void Settings::SaveTo(const std::wstring &iniPath) const { ForEachSetting(IniWriter(iniPath), *this); }

} // namespace pd
