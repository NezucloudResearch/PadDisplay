// PadDisplay by Nezucloud. Copyright (c) 2026 Nezucloud.
// SPDX-License-Identifier: MIT
#include "micbackend.h"
#include "cablesink.h"
#include "log.h"
#include "vdd.h"

#include <windows.h>
#include <cwctype>

namespace pd {
namespace {

std::wstring DriverPackageDir() {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring s = exe;
    return s.substr(0, s.find_last_of(L'\\')) + L"\\driver\\mic";
}

bool Exists(const std::wstring& path) { return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

// A package is shipped (the three files) and Windows will load it: test-signing is on, or Microsoft signed the catalog.
bool PackageLoadsHere() {
    const std::wstring dir = DriverPackageDir();
    if (!Exists(dir + L"\\padmic.inf") || !Exists(dir + L"\\padmic.sys") || !Exists(dir + L"\\padmic.cat")) return false;
    return TestSigningOn() || SignedByMicrosoftHardware(dir + L"\\padmic.cat");
}

} // namespace

MicChoice ChooseMicBackend(const Settings& settings) {
    MicChoice c;
    std::wstring mode = settings.micBackend;
    for (auto& ch : mode) ch = static_cast<wchar_t>(towlower(ch));
    const VddState driver = QueryMic();
    c.driverInstalled = driver == VddState::Ready || driver == VddState::Disabled;
    c.driverLoadable = c.driverInstalled || PackageLoadsHere();
    c.cableInstalled = CableInstalled(settings.micDevice);

    if (mode == L"driver") c.backend = c.driverLoadable ? MicBackend::Driver : MicBackend::None;
    else if (mode == L"cable") c.backend = c.cableInstalled ? MicBackend::Cable : MicBackend::None;
    else if (c.driverLoadable) c.backend = MicBackend::Driver;
    else if (c.cableInstalled) c.backend = MicBackend::Cable;
    c.detail = std::wstring(L"mic backend ") + (c.backend == MicBackend::Driver ? L"PadDisplay driver" : c.backend == MicBackend::Cable ? L"VB-CABLE" : L"none") +
               L" (mode " + mode + L", driver " + (c.driverInstalled ? L"installed" : c.driverLoadable ? L"can be installed" : L"cannot load here") +
               L", VB-CABLE " + (c.cableInstalled ? L"installed" : L"not installed") + L")";
    return c;
}

} // namespace pd
