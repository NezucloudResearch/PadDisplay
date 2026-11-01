// PadDisplay by Nezucloud. Copyright (c) 2026 Nezucloud.
// SPDX-License-Identifier: MIT
#include "gpu_preference.h"

#include "capture/adapters.h"
#include "encoder/gpu_select.h"
#include "exepath.h"
#include "log.h"

#include <windows.h>
#include <vector>

namespace pd
{
    namespace
    {

        constexpr wchar_t kUserGpuPreferences[] = L"Software\\Microsoft\\DirectX\\UserGpuPreferences";

        // Desktop Duplication, the fast way to capture (changed rectangles only), works when the GPU Windows runs this program on is the one
        // that renders the virtual monitor. The tray's "Encode on" says which GPU that is, so the program's GPU preference follows it: 1 =
        // power saving (the integrated GPU), 2 = high performance (a discrete one). When the two differ anyway (Auto moving to the other GPU
        // for gaming or battery) the session captures with Windows.Graphics.Capture instead: slower, but nothing breaks and nothing restarts.
        // With one GPU there is no choice: the GPU that renders the monitor, when it is on the desktop (0 = nothing to say).
        int WantedGpuPreference(const Settings &settings, const std::wstring &gdiName)
        {
            const std::vector<EncodeGpu> gpus = ListEncodeGpus();
            if (gpus.size() >= 2)
            {
                // The usual situation for Auto: on mains, no game (gaming mode and battery are decided per connection, and cannot be known now).
                return ChooseEncodeGpu(gpus, settings.gpu, GpuSituation{}).gpu.integrated ? 1 : 2;
            }
            CaptureTarget t;
            if (gdiName.empty() || !FindOutput(gdiName, t))
                return 0;
            return t.vendorId == 0x8086 ? 1 : 2;
        }

    } // namespace

    bool EnsureGpuPreference(const Settings &settings, const std::wstring &gdiName)
    {
        const int wanted = WantedGpuPreference(settings, gdiName);
        if (wanted == 0)
            return false;
        const wchar_t *want = wanted == 1 ? L"GpuPreference=1;" : L"GpuPreference=2;";
        const std::wstring exe = ThisExePath();
        HKEY key;
        if (RegCreateKeyExW(HKEY_CURRENT_USER, kUserGpuPreferences, 0, nullptr, 0, KEY_READ | KEY_WRITE, nullptr, &key, nullptr) != ERROR_SUCCESS)
            return false;
        wchar_t cur[64] = {};
        DWORD size = sizeof(cur);
        const bool same = RegGetValueW(key, nullptr, exe.c_str(), RRF_RT_REG_SZ, nullptr, cur, &size) == ERROR_SUCCESS && wcscmp(cur, want) == 0;
        if (!same)
        {
            RegSetValueExW(key, exe.c_str(), 0, REG_SZ, reinterpret_cast<const BYTE *>(want), DWORD((wcslen(want) + 1) * sizeof(wchar_t)));
            LOGI("gpu preference set to %ls for %ls", want, exe.c_str());
        }
        RegCloseKey(key);
        return !same;
    }

    bool GpuPreferenceStored()
    {
        wchar_t cur[64] = {};
        DWORD size = sizeof(cur);
        return RegGetValueW(HKEY_CURRENT_USER, kUserGpuPreferences, ThisExePath().c_str(), RRF_RT_REG_SZ, nullptr, cur, &size) == ERROR_SUCCESS;
    }

    void ForgetGpuPreference() { RegDeleteKeyValueW(HKEY_CURRENT_USER, kUserGpuPreferences, ThisExePath().c_str()); }

} // namespace pd
