// PadDisplay by Nezucloud. Copyright (c) 2026 Nezucloud.
// SPDX-License-Identifier: MIT
#include "setup.h"
#include "log.h"
#include "vdd.h"

#include <windows.h>
#include <aclapi.h>
#include <sddl.h>
#include <taskschd.h>
#include <wincrypt.h>
#include <wrl/client.h>
#include <sstream>
#include <vector>

namespace pd {
namespace {

constexpr wchar_t kRecordsKey[] = L"SOFTWARE\\PadDisplay";
constexpr wchar_t kTrustedValue[] = L"TrustedPublishers"; // SHA-1 thumbprints PadDisplay added, separated by spaces
constexpr DWORD kCertEncoding = X509_ASN_ENCODING | PKCS_7_ASN_ENCODING;

// The catalogs whose signer may be trusted: exactly the shipped files. MttVDD (Virtual Display Driver) is signed by
// SignPath Foundation; ViGEmBus and VB-CABLE are signed by Microsoft's hardware publisher, which Windows trusts already.
struct PinnedCatalog {
    const wchar_t* file;
    const char* sha256;
};
const PinnedCatalog kPublisherCatalogs[] = {
    {L"vdd\\MttVDD.cat", "08a0093fc9b2e32b287a6f8a77ca4de0a31830d29fc33d2b13a918dc859468f6"},
};

std::wstring ReadRecord(const wchar_t* name) {
    wchar_t text[1024] = {};
    DWORD size = sizeof(text);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, kRecordsKey, name, RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY, nullptr, text, &size) != ERROR_SUCCESS) return {};
    return text;
}

void WriteRecord(const wchar_t* name, const std::wstring& text) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, kRecordsKey, 0, nullptr, 0, KEY_WRITE | KEY_WOW64_64KEY, nullptr, &key, nullptr) != ERROR_SUCCESS) return;
    if (text.empty()) RegDeleteValueW(key, name);
    else RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(text.c_str()), DWORD((text.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
}

// --- who may change a file ---------------------------------------------------------------------

constexpr DWORD kChangeRights = FILE_WRITE_DATA | FILE_APPEND_DATA | DELETE | WRITE_DAC | WRITE_OWNER | GENERIC_WRITE | GENERIC_ALL;
constexpr DWORD kReplaceChildRights = FILE_DELETE_CHILD | DELETE | WRITE_DAC | WRITE_OWNER | GENERIC_ALL;

// Windows itself, the Administrators group and the Windows Modules Installer: who owns and writes Program Files.
bool IsAdministratorSid(PSID sid) {
    if (IsWellKnownSid(sid, WinLocalSystemSid) || IsWellKnownSid(sid, WinBuiltinAdministratorsSid)) return true;
    PSID trustedInstaller = nullptr;
    if (!ConvertStringSidToSidW(L"S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464", &trustedInstaller)) return false;
    const bool same = EqualSid(sid, trustedInstaller) != FALSE;
    LocalFree(trustedInstaller);
    return same;
}

// Is the owner an administrator, and does the access list give these rights to administrators only?
bool OnlyAdminsHold(const std::wstring& path, DWORD rights) {
    PSID owner = nullptr;
    PACL dacl = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (GetNamedSecurityInfoW(path.c_str(), SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION, &owner, nullptr, &dacl,
                              nullptr, &descriptor) != ERROR_SUCCESS)
        return false;
    bool protectedPath = owner && dacl && IsAdministratorSid(owner); // no access list at all means everyone may do everything
    for (DWORD i = 0; protectedPath && i < dacl->AceCount; ++i) {
        ACE_HEADER* header = nullptr;
        if (!GetAce(dacl, i, reinterpret_cast<void**>(&header))) {
            protectedPath = false;
            break;
        }
        if (header->AceType != ACCESS_ALLOWED_ACE_TYPE || (header->AceFlags & INHERIT_ONLY_ACE)) continue;
        auto* allowed = reinterpret_cast<ACCESS_ALLOWED_ACE*>(header);
        if ((allowed->Mask & rights) && !IsAdministratorSid(&allowed->SidStart)) protectedPath = false;
    }
    LocalFree(descriptor);
    return protectedPath;
}

// "C:\a\b" -> "C:\a" -> "C:\" -> "".
std::wstring ParentOf(const std::wstring& path) {
    if (path.size() <= 3) return {};
    const size_t slash = path.find_last_of(L'\\');
    if (slash == std::wstring::npos) return {};
    return path.substr(0, slash == 2 ? 3 : slash);
}

// --- the Trusted Publishers store --------------------------------------------------------------

HCERTSTORE OpenTrustedPublishers() {
    return CertOpenStore(CERT_STORE_PROV_SYSTEM_W, 0, 0, CERT_SYSTEM_STORE_LOCAL_MACHINE, L"TrustedPublisher");
}

std::wstring Thumbprint(PCCERT_CONTEXT certificate) {
    BYTE hash[20] = {};
    DWORD size = sizeof(hash);
    if (!CertGetCertificateContextProperty(certificate, CERT_SHA1_HASH_PROP_ID, hash, &size)) return {};
    static const wchar_t kDigits[] = L"0123456789ABCDEF";
    std::wstring hex;
    for (DWORD i = 0; i < size; ++i) {
        hex += kDigits[hash[i] >> 4];
        hex += kDigits[hash[i] & 15];
    }
    return hex;
}

std::vector<BYTE> BytesOfHex(const std::wstring& hex) {
    std::vector<BYTE> bytes;
    for (size_t i = 0; i + 1 < hex.size(); i += 2) bytes.push_back(BYTE(wcstoul(hex.substr(i, 2).c_str(), nullptr, 16)));
    return bytes;
}

// Adds the signer of one shipped catalog. added collects the thumbprints that were not in the store before.
bool TrustSignerOf(HCERTSTORE store, const std::wstring& catalog, const char* sha256, std::wstring& added, std::wstring& message) {
    std::string hex;
    if (!Sha256Hex(catalog, hex) || hex != sha256) {
        message += L"Not trusted, because it is not the file PadDisplay shipped: " + catalog + L". ";
        LOGI("trust: hash mismatch for %ls (%s)", catalog.c_str(), hex.c_str());
        return false;
    }
    std::vector<unsigned char> der;
    std::wstring name;
    if (!HasValidSignature(catalog) || !SignerCertificate(catalog, der, name)) {
        message += L"Not trusted, because its signature is not valid: " + catalog + L". ";
        return false;
    }
    PCCERT_CONTEXT signer = CertCreateCertificateContext(kCertEncoding, der.data(), DWORD(der.size()));
    if (!signer) return false;
    bool ok = true;
    if (PCCERT_CONTEXT existing = CertFindCertificateInStore(store, kCertEncoding, 0, CERT_FIND_EXISTING, signer, nullptr)) {
        CertFreeCertificateContext(existing);
        message += name + L" was trusted as a driver publisher already. ";
    } else if (CertAddCertificateContextToStore(store, signer, CERT_STORE_ADD_NEW, nullptr)) {
        added += (added.empty() ? L"" : L" ") + Thumbprint(signer);
        message += name + L" is now trusted as a driver publisher. ";
    } else {
        message += L"Could not add " + name + L" to the Trusted Publishers (error " + std::to_wstring(GetLastError()) + L"). ";
        ok = false;
    }
    CertFreeCertificateContext(signer);
    return ok;
}

struct Bstr {
    explicit Bstr(const wchar_t* text) : value(SysAllocString(text)) {}
    ~Bstr() { SysFreeString(value); }
    Bstr(const Bstr&) = delete;
    Bstr& operator=(const Bstr&) = delete;
    BSTR value;
};

} // namespace

void MarkAddedByPadDisplay(const wchar_t* component, bool added) { WriteRecord(component, added ? L"1" : L""); }

bool AddedByPadDisplay(const wchar_t* component) { return ReadRecord(component) == L"1"; }

void ForgetSetupRecords() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kRecordsKey, 0, KEY_READ | KEY_WOW64_64KEY, &key) != ERROR_SUCCESS) return;
    DWORD subkeys = 0, values = 0;
    RegQueryInfoKeyW(key, nullptr, nullptr, nullptr, &subkeys, nullptr, nullptr, &values, nullptr, nullptr, nullptr, nullptr);
    RegCloseKey(key);
    if (subkeys == 0 && values == 0) RegDeleteKeyExW(HKEY_LOCAL_MACHINE, kRecordsKey, KEY_WOW64_64KEY, 0);
    else LOGI("setup: %lu record(s) left in HKLM\\%ls (something could not be removed)", values, kRecordsKey);
}

bool OnlyAdminsCanChange(const std::wstring& path) {
    if (!OnlyAdminsHold(path, kChangeRights)) return false;
    std::wstring folder = ParentOf(path);
    if (folder.empty() || !OnlyAdminsHold(folder, kChangeRights | FILE_DELETE_CHILD)) return false; // also: adding a DLL next to it
    for (folder = ParentOf(folder); !folder.empty(); folder = ParentOf(folder))
        if (!OnlyAdminsHold(folder, kReplaceChildRights)) return false; // renaming a folder on the way swaps everything below it
    return true;
}

bool TrustDriverPublishers(const std::wstring& driverDir, std::wstring& message) {
    HCERTSTORE store = OpenTrustedPublishers();
    if (!store) {
        message = L"Could not open the Trusted Publishers store (error " + std::to_wstring(GetLastError()) + L").";
        return false;
    }
    std::wstring added = ReadRecord(kTrustedValue);
    bool ok = true;
    for (const auto& catalog : kPublisherCatalogs) ok = TrustSignerOf(store, driverDir + L"\\" + catalog.file, catalog.sha256, added, message) && ok;
    WriteRecord(kTrustedValue, added);
    CertCloseStore(store, 0);
    return ok;
}

bool UntrustDriverPublishers(std::wstring& message) {
    const std::wstring added = ReadRecord(kTrustedValue);
    if (added.empty()) {
        message = L"PadDisplay had not added a driver publisher.";
        return true;
    }
    HCERTSTORE store = OpenTrustedPublishers();
    if (!store) {
        message = L"Could not open the Trusted Publishers store (error " + std::to_wstring(GetLastError()) + L").";
        return false;
    }
    int removed = 0;
    std::wistringstream thumbprints(added);
    for (std::wstring thumbprint; thumbprints >> thumbprint;) {
        std::vector<BYTE> hash = BytesOfHex(thumbprint);
        CRYPT_HASH_BLOB blob{DWORD(hash.size()), hash.data()};
        PCCERT_CONTEXT found = CertFindCertificateInStore(store, kCertEncoding, 0, CERT_FIND_SHA1_HASH, &blob, nullptr);
        if (found && CertDeleteCertificateFromStore(found)) ++removed; // frees found
    }
    CertCloseStore(store, 0);
    WriteRecord(kTrustedValue, L"");
    message = std::to_wstring(removed) + L" driver publisher(s) removed from the Trusted Publishers.";
    return true;
}

void DeleteTaskFolder() {
    const bool initCom = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
    Microsoft::WRL::ComPtr<ITaskService> service;
    Microsoft::WRL::ComPtr<ITaskFolder> root;
    VARIANT none;
    VariantInit(&none);
    HRESULT hr = CoCreateInstance(CLSID_TaskScheduler, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&service));
    if (SUCCEEDED(hr)) hr = service->Connect(none, none, none, none);
    if (SUCCEEDED(hr)) hr = service->GetFolder(Bstr(L"\\").value, &root);
    if (SUCCEEDED(hr)) hr = root->DeleteFolder(Bstr(L"PadDisplay").value, 0); // fails while a task is left in it
    LOGI("setup: task folder \\PadDisplay %s (0x%08lX)", SUCCEEDED(hr) ? "deleted" : "not deleted", static_cast<unsigned long>(hr));
    root.Reset();
    service.Reset();
    if (initCom) CoUninitialize();
}

std::wstring SystemProgram(const wchar_t* name) {
    wchar_t dir[MAX_PATH] = {};
    GetSystemDirectoryW(dir, MAX_PATH);
    return std::wstring(dir) + L"\\" + name;
}

} // namespace pd
