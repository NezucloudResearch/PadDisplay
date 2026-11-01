// PadDisplay by Nezucloud. Copyright (c) 2026 Nezucloud.
// SPDX-License-Identifier: MIT
#include "settings.h"

#include <windows.h>
#include <cstdio>
#include <fstream>
#include <string>

using pd::Settings;

namespace
{

    int failures = 0;

    void Expect(const char *test, bool passed)
    {
        if (passed)
            return;
        std::printf("FAILED %s\n", test);
        ++failures;
    }

    std::wstring TestIniPath()
    {
        wchar_t directory[MAX_PATH];
        GetTempPathW(MAX_PATH, directory);
        return std::wstring(directory) + L"paddisplay_settings_test.ini";
    }

    Settings LoadedFrom(const char *ini)
    {
        const std::wstring path = TestIniPath();
        std::ofstream(path, std::ios::binary) << "[host]\r\n"
                                              << ini;
        Settings settings;
        settings.LoadFrom(path);
        return settings;
    }

    std::string FileText()
    {
        std::ifstream in(TestIniPath(), std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(in), {});
    }

    void ValuesAreClampedToTheirRange()
    {
        const Settings low = LoadedFrom("maxFramesInFlight=0\r\nmaxFps=1\r\nnvencPreset=-3\r\nreconnectGraceSeconds=-1\r\n");
        Expect("frames in flight has a floor", low.maxFramesInFlight == 1);
        Expect("fps has a floor", low.maxFps == 10);
        Expect("preset has a floor", low.nvencPreset == 0);
        Expect("grace has a floor", low.reconnectGraceSeconds == 0);
        const Settings high = LoadedFrom("maxFramesInFlight=99\r\nmaxFps=9999\r\nnvencPreset=50\r\nreconnectGraceSeconds=99999\r\n");
        Expect("frames in flight has a ceiling", high.maxFramesInFlight == 10);
        Expect("fps has a ceiling", high.maxFps == 240);
        Expect("preset has a ceiling", high.nvencPreset == 7);
        Expect("grace has a ceiling", high.reconnectGraceSeconds == 600);
    }

    void AValidPinIsKept()
    {
        Expect("lowest", LoadedFrom("pin=100000\r\n").pin == 100000);
        Expect("highest", LoadedFrom("pin=999999\r\n").pin == 999999);
    }

    void AnInvalidPinIsReplacedWithAValidOne()
    {
        for (const char *ini : {"pin=99999\r\n", "pin=1000000\r\n", "pin=abc\r\n", ""})
        {
            const uint32_t pin = LoadedFrom(ini).pin;
            Expect("a new PIN has six digits", pin >= 100000 && pin <= 999999);
        }
    }

    void TheNewPinIsSaved()
    {
        const uint32_t pin = LoadedFrom("").pin;
        Expect("the file holds the PIN that was generated", FileText().find("pin=" + std::to_string(pin)) != std::string::npos);
    }

    void AMissingKeyKeepsItsDefault()
    {
        const Settings defaults;
        const Settings loaded = LoadedFrom("");
        Expect("number", loaded.usbBitrateMbps == defaults.usbBitrateMbps);
        Expect("flag", loaded.adaptive == defaults.adaptive);
        Expect("text", loaded.codec == defaults.codec);
    }

    void ValuesSurviveASaveAndALoad()
    {
        Settings settings = LoadedFrom("pin=123456\r\n");
        settings.maxFps = 90;
        settings.adaptive = false;
        settings.gpu = L"Some GPU";
        settings.SaveTo(TestIniPath());
        Settings loaded;
        loaded.LoadFrom(TestIniPath());
        Expect("number", loaded.maxFps == 90);
        Expect("flag", !loaded.adaptive);
        Expect("text", loaded.gpu == L"Some GPU");
        Expect("pin", loaded.pin == 123456);
    }

    void TheDiagnosticSwitchIsReadButNeverAddedToTheFile()
    {
        Expect("read", LoadedFrom("profile=1\r\n").profile);
        LoadedFrom("");
        Expect("not added", FileText().find("profile=") == std::string::npos);
    }

} // namespace

int main()
{
    ValuesAreClampedToTheirRange();
    AValidPinIsKept();
    AnInvalidPinIsReplacedWithAValidOne();
    TheNewPinIsSaved();
    AMissingKeyKeepsItsDefault();
    ValuesSurviveASaveAndALoad();
    TheDiagnosticSwitchIsReadButNeverAddedToTheFile();
    DeleteFileW(TestIniPath().c_str());
    if (failures == 0)
        std::puts("settings: all tests passed");
    return failures == 0 ? 0 : 1;
}
