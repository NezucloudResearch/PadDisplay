// PadDisplay by Nezucloud. Copyright (c) 2026 Nezucloud.
// SPDX-License-Identifier: MIT
#include "gpu_profile.h"
#include "log.h"

#include <windows.h>
#include <nvapi.h>
#include <NvApiDriverSettings.h>

namespace pd {
namespace {

const wchar_t kExe[] = L"paddisplay.exe"; // profiles match by file name, any folder
const wchar_t kProfile[] = L"PadDisplay";

void ToNv(NvAPI_UnicodeString dst, const wchar_t* src) {
    size_t i = 0;
    for (; src[i] && i < NVAPI_UNICODE_STRING_MAX - 1; ++i) dst[i] = NvU16(src[i]);
    dst[i] = 0;
}

// RAII DRS session with settings loaded.
struct Session {
    NvDRSSessionHandle h = nullptr;
    bool ok = false;
    Session() {
        if (NvAPI_Initialize() != NVAPI_OK) return;
        ok = NvAPI_DRS_CreateSession(&h) == NVAPI_OK && NvAPI_DRS_LoadSettings(h) == NVAPI_OK;
    }
    ~Session() {
        if (h) NvAPI_DRS_DestroySession(h);
    }
};

NvAPI_Status FindOrCreateProfile(NvDRSSessionHandle s, bool create, NvDRSProfileHandle* prof) {
    NvAPI_UnicodeString name;
    ToNv(name, kExe);
    NVDRS_APPLICATION app{};
    app.version = NVDRS_APPLICATION_VER;
    NvAPI_Status st = NvAPI_DRS_FindApplicationByName(s, name, prof, &app);
    if (st == NVAPI_OK || !create) return st;

    NvAPI_UnicodeString pname;
    ToNv(pname, kProfile);
    st = NvAPI_DRS_FindProfileByName(s, pname, prof);
    if (st != NVAPI_OK) {
        NVDRS_PROFILE p{};
        p.version = NVDRS_PROFILE_VER;
        ToNv(p.profileName, kProfile);
        if ((st = NvAPI_DRS_CreateProfile(s, &p, prof)) != NVAPI_OK) return st;
    }
    NVDRS_APPLICATION a{};
    a.version = NVDRS_APPLICATION_VER;
    ToNv(a.appName, kExe);
    ToNv(a.userFriendlyName, L"PadDisplay");
    return NvAPI_DRS_CreateApplication(s, *prof, &a);
}

std::wstring StatusText(NvAPI_Status st) {
    NvAPI_ShortString text{};
    NvAPI_GetErrorMessage(st, text);
    return Widen(text) + L" (" + std::to_wstring(int(st)) + L")";
}

} // namespace

GpuProfileState QueryNvidiaProfile() {
    Session s;
    if (!s.ok) return GpuProfileState::NoNvidia;
    NvDRSProfileHandle prof = nullptr;
    if (FindOrCreateProfile(s.h, false, &prof) != NVAPI_OK) return GpuProfileState::NotSet;
    NVDRS_SETTING st{};
    st.version = NVDRS_SETTING_VER;
    if (NvAPI_DRS_GetSetting(s.h, prof, PREFERRED_PSTATE_ID, &st) != NVAPI_OK) return GpuProfileState::NotSet;
    return st.u32CurrentValue == PREFERRED_PSTATE_PREFER_MAX ? GpuProfileState::Set : GpuProfileState::NotSet;
}

bool ConfigureNvidiaProfile(bool maxPerformance, std::wstring& message) {
    Session s;
    if (!s.ok) {
        message = L"no NVIDIA driver";
        return true; // nothing to do on other GPUs
    }
    NvDRSProfileHandle prof = nullptr;
    NvAPI_Status st = FindOrCreateProfile(s.h, true, &prof);
    if (st == NVAPI_OK) {
        NVDRS_SETTING set{};
        set.version = NVDRS_SETTING_VER;
        set.settingId = PREFERRED_PSTATE_ID;
        set.settingType = NVDRS_DWORD_TYPE;
        set.u32CurrentValue = maxPerformance ? PREFERRED_PSTATE_PREFER_MAX : PREFERRED_PSTATE_OPTIMAL_POWER;
        st = NvAPI_DRS_SetSetting(s.h, prof, &set);
    }
    if (st == NVAPI_OK) st = NvAPI_DRS_SaveSettings(s.h);
    message = st == NVAPI_OK ? (maxPerformance ? L"NVIDIA profile: prefer maximum performance" : L"NVIDIA profile: optimal power")
                             : L"NVIDIA profile not saved: " + StatusText(st);
    LOGI("gpu: %s", Narrow(message).c_str());
    return st == NVAPI_OK;
}

} // namespace pd
