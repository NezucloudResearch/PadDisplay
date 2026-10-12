// PadDisplay by Nezucloud. Copyright (c) 2026 Nezucloud.
// SPDX-License-Identifier: MIT
#define SECURITY_WIN32
#include "tls.h"
#include "log.h"
#include "protocol.h"
#include "settings.h"

#include <windows.h>
#include <wincrypt.h>
#include <ncrypt.h>
#include <bcrypt.h>
#include <security.h>
#include <schnlsp.h>
#include <algorithm>
#include <cstring>
#include <vector>

namespace pd {

namespace {

constexpr char kFileMagic[] = "PDTLS2";
#ifndef PD_TLS_KEY_NAME
#define PD_TLS_KEY_NAME L"PadDisplay TLS" // tests build with another name so they never touch the real key
#endif
constexpr wchar_t kKeyName[] = PD_TLS_KEY_NAME; // in the Windows user's key store
constexpr size_t kMaxHandshakeBytes = 64 * 1024;

std::wstring IdentityPath() { return AppDataDir() + L"\\tls-identity.bin"; }

std::string HexColon(const uint8_t* p, size_t n) {
    static const char kDigits[] = "0123456789ABCDEF";
    std::string s;
    for (size_t i = 0; i < n; ++i) {
        if (i) s += ':';
        s += kDigits[p[i] >> 4];
        s += kDigits[p[i] & 15];
    }
    return s;
}

bool ReadFileAll(const std::wstring& path, std::vector<uint8_t>& out) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(f, &size) && size.QuadPart > 0 && size.QuadPart < (1 << 20);
    if (ok) {
        out.resize(size_t(size.QuadPart));
        DWORD got = 0;
        ok = ReadFile(f, out.data(), DWORD(out.size()), &got, nullptr) && got == out.size();
    }
    CloseHandle(f);
    return ok;
}

// Written beside the target and renamed over it, so a crash never leaves half an identity behind.
bool WriteFileAtomic(const std::wstring& path, const uint8_t* data, size_t len) {
    std::wstring tmp = path + L".tmp";
    HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD put = 0;
    bool ok = WriteFile(f, data, DWORD(len), &put, nullptr) && put == len && FlushFileBuffers(f);
    CloseHandle(f);
    if (ok) ok = MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
    if (!ok) DeleteFileW(tmp.c_str());
    return ok;
}

bool RawSend(SOCKET s, const uint8_t* p, size_t n) {
    while (n > 0) {
        int sent = send(s, reinterpret_cast<const char*>(p), int(std::min<size_t>(n, 1 << 20)), 0);
        if (sent <= 0) return false;
        p += sent;
        n -= size_t(sent);
    }
    return true;
}

// A self-signed X.509 certificate for a CNG key, built by hand: CertCreateSelfSignCertificate blocks forever when it is
// handed a CNG key handle (seen on Windows 11), whereas signing through CERT_NCRYPT_KEY_SPEC works.
bool BuildSelfSignedCertificate(NCRYPT_KEY_HANDLE key, std::vector<uint8_t>& der) {
    wchar_t host[MAX_COMPUTERNAME_LENGTH + 1] = {};
    DWORD hostLen = DWORD(std::size(host));
    GetComputerNameW(host, &hostLen);
    std::wstring dn = std::wstring(L"CN=PadDisplay ") + host;
    DWORD nameLen = 0;
    CertStrToNameW(X509_ASN_ENCODING, dn.c_str(), CERT_X500_NAME_STR, nullptr, nullptr, &nameLen, nullptr);
    std::vector<uint8_t> name(nameLen);
    if (nameLen == 0 || !CertStrToNameW(X509_ASN_ENCODING, dn.c_str(), CERT_X500_NAME_STR, nullptr, name.data(), &nameLen, nullptr))
        return false;

    DWORD infoLen = 0;
    if (!CryptExportPublicKeyInfoEx(key, CERT_NCRYPT_KEY_SPEC, X509_ASN_ENCODING, nullptr, 0, nullptr, nullptr, &infoLen)) return false;
    std::vector<uint8_t> pubKey(infoLen);
    auto* pubInfo = reinterpret_cast<CERT_PUBLIC_KEY_INFO*>(pubKey.data());
    if (!CryptExportPublicKeyInfoEx(key, CERT_NCRYPT_KEY_SPEC, X509_ASN_ENCODING, nullptr, 0, nullptr, pubInfo, &infoLen)) return false;

    // Tablets pin the fingerprint and ignore the dates; the validity is long so other software that looks does not complain.
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    ULARGE_INTEGER t;
    t.LowPart = now.dwLowDateTime;
    t.HighPart = now.dwHighDateTime;
    constexpr ULONGLONG kDay = 864000000000ULL;
    ULARGE_INTEGER startT, endT;
    startT.QuadPart = t.QuadPart - kDay;
    endT.QuadPart = t.QuadPart + 20 * 365 * kDay;

    uint8_t serial[8];
    if (BCryptGenRandom(nullptr, serial, sizeof(serial), BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) return false;
    serial[7] = uint8_t((serial[7] & 0x7F) | 0x01); // positive and not zero

    CRYPT_ALGORITHM_IDENTIFIER sigAlg{};
    sigAlg.pszObjId = const_cast<LPSTR>(szOID_ECDSA_SHA256);
    CERT_INFO info{};
    info.dwVersion = CERT_V3;
    info.SerialNumber = {sizeof(serial), serial};
    info.SignatureAlgorithm = sigAlg;
    info.Issuer = {nameLen, name.data()};
    info.Subject = info.Issuer;
    info.NotBefore = {startT.LowPart, startT.HighPart};
    info.NotAfter = {endT.LowPart, endT.HighPart};
    info.SubjectPublicKeyInfo = *pubInfo;

    DWORD derLen = 0;
    if (!CryptSignAndEncodeCertificate(key, CERT_NCRYPT_KEY_SPEC, X509_ASN_ENCODING, X509_CERT_TO_BE_SIGNED, &info, &sigAlg, nullptr,
                                       nullptr, &derLen))
        return false;
    der.resize(derLen);
    return CryptSignAndEncodeCertificate(key, CERT_NCRYPT_KEY_SPEC, X509_ASN_ENCODING, X509_CERT_TO_BE_SIGNED, &info, &sigAlg, nullptr,
                                         der.data(), &derLen) != 0;
}

} // namespace

// ---------------------------------------------------------------------------------------------
// One connection.

struct TlsState::Impl {
    SOCKET sock = INVALID_SOCKET;
    CtxtHandle ctx{};
    bool haveCtx = false;
    SecPkgContext_StreamSizes sizes{};

    // Receive side. Ciphertext sits at enc[encStart, encStart + encLen); a decrypted record stays inside enc
    // (DecryptMessage works in place) at [plainPos, plainEnd) until the reader has taken all of it.
    std::vector<uint8_t> enc = std::vector<uint8_t>(48 * 1024);
    size_t encStart = 0, encLen = 0;
    size_t plainPos = 0, plainEnd = 0;

    // Send side: the records of one message, built and encrypted in place. Only ever grows, so a frame
    // costs no allocation once the biggest one has been seen.
    std::vector<uint8_t> out;

    ~Impl() {
        if (haveCtx) DeleteSecurityContext(&ctx);
    }

    // >0 plaintext bytes copied, 0 the peer closed the connection, <0 error.
    int ReadPlain(uint8_t* dst, size_t n) {
        for (;;) {
            if (plainPos < plainEnd) {
                size_t take = std::min(n, plainEnd - plainPos);
                memcpy(dst, enc.data() + plainPos, take);
                plainPos += take;
                return int(take);
            }
            // Nothing is left of the last record, so the buffer may be reused from here on.
            if (encLen > 0) {
                SecBuffer b[4] = {};
                b[0].BufferType = SECBUFFER_DATA;
                b[0].pvBuffer = enc.data() + encStart;
                b[0].cbBuffer = ULONG(encLen);
                for (int i = 1; i < 4; ++i) b[i].BufferType = SECBUFFER_EMPTY;
                SecBufferDesc desc{SECBUFFER_VERSION, 4, b};
                SECURITY_STATUS ss = DecryptMessage(&ctx, &desc, 0, nullptr);
                if (ss == SEC_E_OK) {
                    encLen = 0;
                    for (int i = 1; i < 4; ++i) {
                        if (b[i].BufferType == SECBUFFER_DATA) {
                            plainPos = size_t(static_cast<uint8_t*>(b[i].pvBuffer) - enc.data());
                            plainEnd = plainPos + b[i].cbBuffer;
                        } else if (b[i].BufferType == SECBUFFER_EXTRA) {
                            encStart = size_t(static_cast<uint8_t*>(b[i].pvBuffer) - enc.data());
                            encLen = b[i].cbBuffer;
                        }
                    }
                    continue;
                }
                if (ss == SEC_I_CONTEXT_EXPIRED) return 0; // the peer sent close_notify
                if (ss != SEC_E_INCOMPLETE_MESSAGE) return -1; // includes SEC_I_RENEGOTIATE: we never renegotiate
            }
            // Need more ciphertext (the first bytes of a record, or the rest of one).
            if (encStart > 0) {
                memmove(enc.data(), enc.data() + encStart, encLen);
                encStart = 0;
            }
            if (encLen == enc.size()) return -1; // a record bigger than any valid TLS record
            int got = recv(sock, reinterpret_cast<char*>(enc.data()) + encLen, int(enc.size() - encLen), 0);
            if (got <= 0) return got == 0 ? 0 : -1;
            encLen += size_t(got);
        }
    }
};

TlsState::TlsState() : impl_(std::make_unique<Impl>()) {}
TlsState::~TlsState() = default;

bool TlsState::HasBuffered() const { return impl_->plainPos < impl_->plainEnd || impl_->encLen > 0; }

bool TlsState::RecvAll(void* data, size_t len) {
    auto* p = static_cast<uint8_t*>(data);
    while (len > 0) {
        int n = impl_->ReadPlain(p, len);
        if (n <= 0) return false;
        p += n;
        len -= size_t(n);
    }
    return true;
}

bool TlsState::Send(const void* a, size_t aLen, const void* b, size_t bLen) {
    Impl& m = *impl_;
    const size_t total = aLen + bLen;
    if (total == 0) return true;
    const size_t maxMsg = m.sizes.cbMaximumMessage;
    const size_t overhead = m.sizes.cbHeader + m.sizes.cbTrailer;
    const size_t records = (total + maxMsg - 1) / maxMsg;
    const size_t need = total + records * overhead;
    if (m.out.size() < need) m.out.resize(need);

    const auto* pa = static_cast<const uint8_t*>(a);
    const auto* pb = static_cast<const uint8_t*>(b);
    size_t done = 0, written = 0;
    while (done < total) {
        const size_t n = std::min(maxMsg, total - done);
        uint8_t* rec = m.out.data() + written;
        uint8_t* data = rec + m.sizes.cbHeader;
        // Copy [done, done + n) of the concatenation of a and b into the record.
        size_t copied = 0;
        if (done < aLen) {
            copied = std::min(n, aLen - done);
            memcpy(data, pa + done, copied);
        }
        if (copied < n) memcpy(data + copied, pb + (done + copied - aLen), n - copied);

        SecBuffer bufs[4] = {};
        bufs[0].BufferType = SECBUFFER_STREAM_HEADER;
        bufs[0].pvBuffer = rec;
        bufs[0].cbBuffer = m.sizes.cbHeader;
        bufs[1].BufferType = SECBUFFER_DATA;
        bufs[1].pvBuffer = data;
        bufs[1].cbBuffer = ULONG(n);
        bufs[2].BufferType = SECBUFFER_STREAM_TRAILER;
        bufs[2].pvBuffer = data + n;
        bufs[2].cbBuffer = m.sizes.cbTrailer;
        bufs[3].BufferType = SECBUFFER_EMPTY;
        SecBufferDesc desc{SECBUFFER_VERSION, 4, bufs};
        if (EncryptMessage(&m.ctx, 0, &desc, 0) != SEC_E_OK) return false;
        // The trailer may come out shorter than the room made for it; the next record follows the real end.
        written += size_t(bufs[0].cbBuffer) + bufs[1].cbBuffer + bufs[2].cbBuffer;
        done += n;
    }
    return RawSend(m.sock, m.out.data(), written);
}

// ---------------------------------------------------------------------------------------------
// The host's identity.

struct TlsIdentity::Impl {
    NCRYPT_PROV_HANDLE prov = 0;
    PCCERT_CONTEXT cert = nullptr;
    CredHandle cred{};
    bool haveCred = false;
    std::string fingerprint;

    ~Impl() {
        if (haveCred) FreeCredentialsHandle(&cred);
        if (cert) CertFreeCertificateContext(cert);
        if (prov) NCryptFreeObject(prov);
    }

    // Schannel signs with the key the certificate points to. The key itself stays in the Windows user's key store:
    // non-exportable, so the identity file holds only the public certificate.
    bool PointCertificateAtKey() {
        CRYPT_KEY_PROV_INFO kpi{};
        kpi.pwszContainerName = const_cast<LPWSTR>(kKeyName);
        kpi.pwszProvName = const_cast<LPWSTR>(MS_KEY_STORAGE_PROVIDER);
        return CertSetCertificateContextProperty(cert, CERT_KEY_PROV_INFO_PROP_ID, 0, &kpi) != 0;
    }

    bool Load() {
        std::vector<uint8_t> file;
        const size_t magic = sizeof(kFileMagic) - 1;
        if (!ReadFileAll(IdentityPath(), file) || file.size() < magic + 4 || memcmp(file.data(), kFileMagic, magic) != 0) return false;
        uint32_t derLen = rd32(&file[magic]);
        if (derLen != file.size() - magic - 4) return false;
        NCRYPT_KEY_HANDLE key = 0;
        if (NCryptOpenKey(prov, &key, kKeyName, 0, 0) != ERROR_SUCCESS) return false; // the file outlived its key
        cert = CertCreateCertificateContext(X509_ASN_ENCODING, &file[magic + 4], derLen);
        // The key under this name may have been replaced since the file was written (the file restored from a backup,
        // or deleted and made again): a certificate for another key would make every handshake fail.
        bool same = false;
        DWORD infoLen = 0;
        if (cert && CryptExportPublicKeyInfoEx(key, CERT_NCRYPT_KEY_SPEC, X509_ASN_ENCODING, nullptr, 0, nullptr, nullptr, &infoLen)) {
            std::vector<uint8_t> pubKey(infoLen);
            auto* pubInfo = reinterpret_cast<CERT_PUBLIC_KEY_INFO*>(pubKey.data());
            same = CryptExportPublicKeyInfoEx(key, CERT_NCRYPT_KEY_SPEC, X509_ASN_ENCODING, nullptr, 0, nullptr, pubInfo, &infoLen) &&
                   CertComparePublicKeyInfo(X509_ASN_ENCODING, pubInfo, &cert->pCertInfo->SubjectPublicKeyInfo);
        }
        NCryptFreeObject(key);
        return same && PointCertificateAtKey();
    }

    bool CreateAndSave() {
        // A new key replaces whatever an earlier identity left under this name.
        NCRYPT_KEY_HANDLE key = 0;
        if (NCryptCreatePersistedKey(prov, &key, NCRYPT_ECDSA_P256_ALGORITHM, kKeyName, 0, NCRYPT_OVERWRITE_KEY_FLAG) != ERROR_SUCCESS) return false;
        std::vector<uint8_t> der;
        bool ok = NCryptFinalizeKey(key, 0) == ERROR_SUCCESS && BuildSelfSignedCertificate(key, der);
        NCryptFreeObject(key);
        if (!ok) return false;

        const size_t magic = sizeof(kFileMagic) - 1;
        std::vector<uint8_t> file(magic + 4 + der.size());
        memcpy(file.data(), kFileMagic, magic);
        wr32(&file[magic], uint32_t(der.size()));
        memcpy(&file[magic + 4], der.data(), der.size());
        // An identity that cannot be kept would change at every start and make tablets refuse the PC.
        if (!WriteFileAtomic(IdentityPath(), file.data(), file.size())) return false;
        cert = CertCreateCertificateContext(X509_ASN_ENCODING, der.data(), DWORD(der.size()));
        return cert && PointCertificateAtKey();
    }

    bool Init() {
        if (NCryptOpenStorageProvider(&prov, MS_KEY_STORAGE_PROVIDER, 0) != ERROR_SUCCESS) return false;
        if (Load()) {
            LOGI("tls: loaded the host identity");
        } else {
            if (cert) CertFreeCertificateContext(cert), cert = nullptr;
            if (!CreateAndSave()) {
                LOGI("tls: cannot create the host identity");
                return false;
            }
            LOGI("tls: made a new host identity");
        }
        uint8_t hash[32];
        DWORD hashLen = sizeof(hash);
        if (!CryptHashCertificate2(BCRYPT_SHA256_ALGORITHM, 0, nullptr, cert->pbCertEncoded, cert->cbCertEncoded, hash, &hashLen)) return false;
        fingerprint = HexColon(hash, hashLen);

        // TLS 1.2 only: it is on every Windows 10 build we support, and TLS 1.3 would only save one round trip at connect.
        SCHANNEL_CRED sc{};
        sc.dwVersion = SCHANNEL_CRED_VERSION;
        sc.cCreds = 1;
        sc.paCred = &cert;
        sc.grbitEnabledProtocols = SP_PROT_TLS1_2_SERVER;
        sc.dwFlags = SCH_USE_STRONG_CRYPTO | SCH_CRED_NO_SYSTEM_MAPPER | SCH_CRED_NO_DEFAULT_CREDS;
        TimeStamp expiry;
        SECURITY_STATUS ss = AcquireCredentialsHandleW(nullptr, const_cast<LPWSTR>(UNISP_NAME_W), SECPKG_CRED_INBOUND, nullptr, &sc, nullptr,
                                                       nullptr, &cred, &expiry);
        if (ss != SEC_E_OK) {
            LOGI("tls: AcquireCredentialsHandle failed 0x%08lx", static_cast<unsigned long>(ss));
            return false;
        }
        haveCred = true;
        return true;
    }
};

TlsIdentity::TlsIdentity() : impl_(std::make_unique<Impl>()) {}
TlsIdentity::~TlsIdentity() = default;

const std::string& TlsIdentity::Fingerprint() const { return impl_->fingerprint; }

std::unique_ptr<TlsIdentity> TlsIdentity::LoadOrCreate() {
    std::unique_ptr<TlsIdentity> id(new TlsIdentity());
    if (!id->impl_->Init()) return nullptr;
    return id;
}

std::shared_ptr<TlsState> TlsIdentity::Accept(SOCKET s, std::string* info) {
    std::shared_ptr<TlsState> st(new TlsState());
    TlsState::Impl& m = *st->impl_;
    m.sock = s;

    std::vector<uint8_t> in;
    bool needData = true, first = true;
    for (;;) {
        if (needData) {
            uint8_t buf[16 * 1024];
            int got = recv(s, reinterpret_cast<char*>(buf), int(sizeof(buf)), 0);
            if (got <= 0) return nullptr;
            in.insert(in.end(), buf, buf + got);
            needData = false;
            if (in.size() > kMaxHandshakeBytes) return nullptr;
        }
        SecBuffer inb[2] = {};
        inb[0].BufferType = SECBUFFER_TOKEN;
        inb[0].pvBuffer = in.data();
        inb[0].cbBuffer = ULONG(in.size());
        inb[1].BufferType = SECBUFFER_EMPTY;
        SecBuffer outb[1] = {};
        outb[0].BufferType = SECBUFFER_TOKEN;
        SecBufferDesc inDesc{SECBUFFER_VERSION, 2, inb}, outDesc{SECBUFFER_VERSION, 1, outb};
        ULONG attrs = 0;
        TimeStamp expiry;
        SECURITY_STATUS ss = AcceptSecurityContext(&impl_->cred, first ? nullptr : &m.ctx, &inDesc,
                                                   ASC_REQ_SEQUENCE_DETECT | ASC_REQ_REPLAY_DETECT | ASC_REQ_CONFIDENTIALITY |
                                                       ASC_REQ_EXTENDED_ERROR | ASC_REQ_ALLOCATE_MEMORY | ASC_REQ_STREAM,
                                                   0, &m.ctx, &outDesc, &attrs, &expiry);
        if (ss != SEC_E_INCOMPLETE_MESSAGE) {
            first = false;
            m.haveCtx = true; // from here on a context exists (even when the call failed) and is freed with the state
        }
        bool sent = true;
        if (outb[0].cbBuffer > 0 && outb[0].pvBuffer) {
            sent = RawSend(s, static_cast<uint8_t*>(outb[0].pvBuffer), outb[0].cbBuffer);
            FreeContextBuffer(outb[0].pvBuffer);
        }
        if (!sent) return nullptr;

        if (ss == SEC_E_INCOMPLETE_MESSAGE) {
            needData = true;
            continue;
        }
        if (ss != SEC_E_OK && ss != SEC_I_CONTINUE_NEEDED) {
            LOGI("tls: handshake failed 0x%08lx", static_cast<unsigned long>(ss));
            return nullptr;
        }
        // Bytes the client sent beyond what this step used belong to the next step (or, once done, to the stream).
        size_t extra = inb[1].BufferType == SECBUFFER_EXTRA ? inb[1].cbBuffer : 0;
        std::vector<uint8_t> rest(in.end() - std::ptrdiff_t(extra), in.end());
        in = std::move(rest);
        if (ss == SEC_E_OK) {
            if (QueryContextAttributesW(&m.ctx, SECPKG_ATTR_STREAM_SIZES, &m.sizes) != SEC_E_OK) return nullptr;
            if (in.size() > m.enc.size()) return nullptr;
            memcpy(m.enc.data(), in.data(), in.size());
            m.encLen = in.size();
            if (info) {
                SecPkgContext_ConnectionInfo ci{};
                if (QueryContextAttributesW(&m.ctx, SECPKG_ATTR_CONNECTION_INFO, &ci) == SEC_E_OK)
                    *info = std::string(ci.dwProtocol & SP_PROT_TLS1_2 ? "TLS 1.2" : "TLS") + ", " + std::to_string(ci.dwCipherStrength) + "-bit";
            }
            return st;
        }
        needData = in.empty();
    }
}

} // namespace pd
