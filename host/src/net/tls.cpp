// PadDisplay by Nezucloud. Copyright (c) 2026 Nezucloud.
// SPDX-License-Identifier: MIT
#define SECURITY_WIN32
#include "tls.h"
#include "log.h"
#include "net.h"
#include "protocol.h"
#include "settings.h"

#include <windows.h>
#include <wincrypt.h>
#include <ncrypt.h>
#include <bcrypt.h>
#include <security.h>
#include <schnlsp.h>
#include <aclapi.h>
#include <sddl.h>
#include <algorithm>
#include <cstring>
#include <vector>

namespace pd
{

    namespace
    {

        constexpr char kFileMagic[] = "PDTLS2";
        constexpr size_t kMagicBytes = sizeof(kFileMagic) - 1;
        constexpr size_t kFileHeaderBytes = kMagicBytes + sizeof(uint32_t); // then the certificate's length, then the certificate
#ifndef PD_TLS_KEY_NAME
#define PD_TLS_KEY_NAME L"PadDisplay TLS" // tests build with another name so they never touch the real key
#endif
        constexpr wchar_t kKeyName[] = PD_TLS_KEY_NAME; // in the Windows user's key store
        constexpr size_t kMaxHandshakeBytes = 64 * 1024;

        std::wstring IdentityPath() { return AppDataDir() + L"\\tls-identity.bin"; }

        // The machine-wide certificate lives beside the service's machine config, under %ProgramData%\PadDisplay,
        // readable by everyone (the certificate is public) but writable only by SYSTEM + administrators.
        std::wstring MachineIdentityPath() { return MachineDataDir() + L"\\tls-identity.bin"; }

        // Grant the machine key to the signed-in user as well as SYSTEM and administrators, so the non-elevated tray
        // host can sign with the same key the SYSTEM pre-sign-in service uses. Without this a machine key is usable
        // only by its creator and SYSTEM, and the tray host's handshakes would fail.
        void GrantMachineKeyToUsers(NCRYPT_KEY_HANDLE key)
        {
            PSECURITY_DESCRIPTOR sd = nullptr;
            if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GR;;;AU)",
                                                                      SDDL_REVISION_1, &sd, nullptr))
                return;
            DWORD len = GetSecurityDescriptorLength(sd);
            NCryptSetProperty(key, NCRYPT_SECURITY_DESCR_PROPERTY, static_cast<PBYTE>(sd), len,
                              NCRYPT_SILENT_FLAG | DACL_SECURITY_INFORMATION);
            LocalFree(sd);
        }

        // Lock the machine certificate file to SYSTEM + administrators (full) and everyone else to read: the public
        // certificate must be readable by the tray host but tamper-proof to a standard user.
        void RestrictMachineFile(const std::wstring &path)
        {
            PSECURITY_DESCRIPTOR sd = nullptr;
            if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;;FA;;;SY)(A;;FA;;;BA)(A;;FR;;;AU)",
                                                                      SDDL_REVISION_1, &sd, nullptr))
                return;
            BOOL present = FALSE, defaulted = FALSE;
            PACL dacl = nullptr;
            if (GetSecurityDescriptorDacl(sd, &present, &dacl, &defaulted) && present)
                SetNamedSecurityInfoW(const_cast<wchar_t *>(path.c_str()), SE_FILE_OBJECT,
                                      DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, nullptr, nullptr, dacl, nullptr);
            LocalFree(sd);
        }

        std::string HexColon(const uint8_t *p, size_t n)
        {
            static const char kDigits[] = "0123456789ABCDEF";
            std::string s;
            for (size_t i = 0; i < n; ++i)
            {
                if (i)
                    s += ':';
                s += kDigits[p[i] >> 4];
                s += kDigits[p[i] & 15];
            }
            return s;
        }

        bool ReadFileAll(const std::wstring &path, std::vector<uint8_t> &out)
        {
            HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (f == INVALID_HANDLE_VALUE)
                return false;
            LARGE_INTEGER size{};
            bool ok = GetFileSizeEx(f, &size) && size.QuadPart > 0 && size.QuadPart < (1 << 20);
            if (ok)
            {
                out.resize(size_t(size.QuadPart));
                DWORD got = 0;
                ok = ReadFile(f, out.data(), DWORD(out.size()), &got, nullptr) && got == out.size();
            }
            CloseHandle(f);
            return ok;
        }

        // Written beside the target and renamed over it, so a crash never leaves half an identity behind.
        bool WriteFileAtomic(const std::wstring &path, const uint8_t *data, size_t len)
        {
            std::wstring tmp = path + L".tmp";
            HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (f == INVALID_HANDLE_VALUE)
                return false;
            DWORD put = 0;
            bool ok = WriteFile(f, data, DWORD(len), &put, nullptr) && put == len && FlushFileBuffers(f);
            CloseHandle(f);
            if (ok)
                ok = MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
            if (!ok)
                DeleteFileW(tmp.c_str());
            return ok;
        }

        // The certificate in an identity file. Empty = there is none, or the file is not one of ours.
        std::vector<uint8_t> ReadIdentityFile(const std::wstring &path)
        {
            std::vector<uint8_t> file;
            if (!ReadFileAll(path, file) || file.size() < kFileHeaderBytes || memcmp(file.data(), kFileMagic, kMagicBytes) != 0)
                return {};
            if (rd32(&file[kMagicBytes]) != file.size() - kFileHeaderBytes)
                return {};
            return std::vector<uint8_t>(file.begin() + kFileHeaderBytes, file.end());
        }

        bool WriteIdentityFile(const std::wstring &path, const std::vector<uint8_t> &der)
        {
            std::vector<uint8_t> file(kFileHeaderBytes + der.size());
            memcpy(file.data(), kFileMagic, kMagicBytes);
            wr32(&file[kMagicBytes], uint32_t(der.size()));
            memcpy(&file[kFileHeaderBytes], der.data(), der.size());
            return WriteFileAtomic(path, file.data(), file.size());
        }

        // The public half of a key as a CERT_PUBLIC_KEY_INFO, which holds pointers into its own memory. Empty = failed.
        std::vector<uint8_t> ExportPublicKey(NCRYPT_KEY_HANDLE key)
        {
            DWORD size = 0;
            if (!CryptExportPublicKeyInfoEx(key, CERT_NCRYPT_KEY_SPEC, X509_ASN_ENCODING, nullptr, 0, nullptr, nullptr, &size))
                return {};
            std::vector<uint8_t> info(size);
            if (!CryptExportPublicKeyInfoEx(key, CERT_NCRYPT_KEY_SPEC, X509_ASN_ENCODING, nullptr, 0, nullptr, reinterpret_cast<CERT_PUBLIC_KEY_INFO *>(info.data()), &size))
                return {};
            return info;
        }

        bool CertificateIsForKey(PCCERT_CONTEXT cert, NCRYPT_KEY_HANDLE key)
        {
            const std::vector<uint8_t> publicKey = ExportPublicKey(key);
            return !publicKey.empty() && CertComparePublicKeyInfo(X509_ASN_ENCODING, const_cast<CERT_PUBLIC_KEY_INFO *>(reinterpret_cast<const CERT_PUBLIC_KEY_INFO *>(publicKey.data())),
                                                                  &cert->pCertInfo->SubjectPublicKeyInfo);
        }

        // "CN=PadDisplay <computer>", DER encoded. Empty = failed.
        std::vector<uint8_t> NameOfThisPc()
        {
            wchar_t host[MAX_COMPUTERNAME_LENGTH + 1] = {};
            DWORD hostLen = DWORD(std::size(host));
            GetComputerNameW(host, &hostLen);
            const std::wstring dn = std::wstring(L"CN=PadDisplay ") + host;
            DWORD nameLen = 0;
            CertStrToNameW(X509_ASN_ENCODING, dn.c_str(), CERT_X500_NAME_STR, nullptr, nullptr, &nameLen, nullptr);
            std::vector<uint8_t> name(nameLen);
            if (nameLen == 0 || !CertStrToNameW(X509_ASN_ENCODING, dn.c_str(), CERT_X500_NAME_STR, nullptr, name.data(), &nameLen, nullptr))
                return {};
            name.resize(nameLen);
            return name;
        }

        // Tablets pin the fingerprint and ignore the dates; the validity is long so other software that looks does not complain.
        void SetLongValidity(CERT_INFO &info)
        {
            constexpr ULONGLONG kDay = 864000000000ULL; // in FILETIME's 100 ns
            FILETIME now;
            GetSystemTimeAsFileTime(&now);
            ULARGE_INTEGER t;
            t.LowPart = now.dwLowDateTime;
            t.HighPart = now.dwHighDateTime;
            ULARGE_INTEGER start, end;
            start.QuadPart = t.QuadPart - kDay;
            end.QuadPart = t.QuadPart + 20 * 365 * kDay;
            info.NotBefore = {start.LowPart, start.HighPart};
            info.NotAfter = {end.LowPart, end.HighPart};
        }

        bool SignCertificate(NCRYPT_KEY_HANDLE key, CERT_INFO &info, std::vector<uint8_t> &der)
        {
            DWORD derLen = 0;
            if (!CryptSignAndEncodeCertificate(key, CERT_NCRYPT_KEY_SPEC, X509_ASN_ENCODING, X509_CERT_TO_BE_SIGNED, &info, &info.SignatureAlgorithm, nullptr, nullptr, &derLen))
                return false;
            der.resize(derLen);
            return CryptSignAndEncodeCertificate(key, CERT_NCRYPT_KEY_SPEC, X509_ASN_ENCODING, X509_CERT_TO_BE_SIGNED, &info, &info.SignatureAlgorithm, nullptr, der.data(), &derLen) != 0;
        }

        // A self-signed X.509 certificate for a CNG key, built by hand: CertCreateSelfSignCertificate blocks forever when it is
        // handed a CNG key handle (seen on Windows 11), whereas signing through CERT_NCRYPT_KEY_SPEC works.
        bool BuildSelfSignedCertificate(NCRYPT_KEY_HANDLE key, std::vector<uint8_t> &der)
        {
            std::vector<uint8_t> name = NameOfThisPc();
            std::vector<uint8_t> publicKey = ExportPublicKey(key);
            if (name.empty() || publicKey.empty())
                return false;

            uint8_t serial[8];
            if (BCryptGenRandom(nullptr, serial, sizeof(serial), BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0)
                return false;
            serial[7] = uint8_t((serial[7] & 0x7F) | 0x01); // positive and not zero

            CERT_INFO info{};
            info.dwVersion = CERT_V3;
            info.SerialNumber = {sizeof(serial), serial};
            info.SignatureAlgorithm.pszObjId = const_cast<LPSTR>(szOID_ECDSA_SHA256);
            info.Issuer = {DWORD(name.size()), name.data()};
            info.Subject = info.Issuer;
            SetLongValidity(info);
            info.SubjectPublicKeyInfo = *reinterpret_cast<CERT_PUBLIC_KEY_INFO *>(publicKey.data());
            return SignCertificate(key, info, der);
        }

    } // namespace

    // ---------------------------------------------------------------------------------------------
    // One connection.

    struct TlsState::Impl
    {
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

        ~Impl()
        {
            if (haveCtx)
                DeleteSecurityContext(&ctx);
        }

        // >0 plaintext bytes copied, 0 the peer closed the connection, <0 error.
        int ReadPlain(uint8_t *dst, size_t n)
        {
            for (;;)
            {
                if (plainPos < plainEnd)
                    return TakePlain(dst, n);
                // Nothing is left of the last record, so the buffer may be reused from here on.
                if (encLen > 0)
                {
                    const SECURITY_STATUS ss = DecryptBuffered();
                    if (ss == SEC_E_OK)
                        continue;
                    if (ss == SEC_I_CONTEXT_EXPIRED)
                        return 0; // the peer sent close_notify
                    if (ss != SEC_E_INCOMPLETE_MESSAGE)
                        return -1; // includes SEC_I_RENEGOTIATE: we never renegotiate
                }
                const int got = ReceiveCiphertext();
                if (got <= 0)
                    return got;
            }
        }

        int TakePlain(uint8_t *dst, size_t n)
        {
            const size_t take = std::min(n, plainEnd - plainPos);
            memcpy(dst, enc.data() + plainPos, take);
            plainPos += take;
            return int(take);
        }

        SECURITY_STATUS DecryptBuffered()
        {
            SecBuffer b[4] = {};
            b[0].BufferType = SECBUFFER_DATA;
            b[0].pvBuffer = enc.data() + encStart;
            b[0].cbBuffer = ULONG(encLen);
            for (int i = 1; i < 4; ++i)
                b[i].BufferType = SECBUFFER_EMPTY;
            SecBufferDesc desc{SECBUFFER_VERSION, 4, b};
            const SECURITY_STATUS ss = DecryptMessage(&ctx, &desc, 0, nullptr);
            if (ss != SEC_E_OK)
                return ss;
            encLen = 0;
            for (int i = 1; i < 4; ++i)
            {
                if (b[i].BufferType == SECBUFFER_DATA)
                {
                    plainPos = size_t(static_cast<uint8_t *>(b[i].pvBuffer) - enc.data());
                    plainEnd = plainPos + b[i].cbBuffer;
                }
                else if (b[i].BufferType == SECBUFFER_EXTRA)
                {
                    encStart = size_t(static_cast<uint8_t *>(b[i].pvBuffer) - enc.data());
                    encLen = b[i].cbBuffer;
                }
            }
            return SEC_E_OK;
        }

        // For the first bytes of a record, or the rest of one. >0 bytes received, 0 the peer closed the connection, <0 error.
        int ReceiveCiphertext()
        {
            if (encStart > 0)
            {
                memmove(enc.data(), enc.data() + encStart, encLen);
                encStart = 0;
            }
            if (encLen == enc.size())
                return -1; // a record bigger than any valid TLS record
            const int got = recv(sock, reinterpret_cast<char *>(enc.data()) + encLen, int(enc.size() - encLen), 0);
            if (got <= 0)
                return got == 0 ? 0 : -1;
            encLen += size_t(got);
            return got;
        }

        // Encrypts in place the n bytes that follow the room for the header at rec. false = failed; recordBytes = its real size.
        bool EncryptRecord(uint8_t *rec, size_t n, size_t &recordBytes)
        {
            uint8_t *data = rec + sizes.cbHeader;
            SecBuffer bufs[4] = {};
            bufs[0].BufferType = SECBUFFER_STREAM_HEADER;
            bufs[0].pvBuffer = rec;
            bufs[0].cbBuffer = sizes.cbHeader;
            bufs[1].BufferType = SECBUFFER_DATA;
            bufs[1].pvBuffer = data;
            bufs[1].cbBuffer = ULONG(n);
            bufs[2].BufferType = SECBUFFER_STREAM_TRAILER;
            bufs[2].pvBuffer = data + n;
            bufs[2].cbBuffer = sizes.cbTrailer;
            bufs[3].BufferType = SECBUFFER_EMPTY;
            SecBufferDesc desc{SECBUFFER_VERSION, 4, bufs};
            if (EncryptMessage(&ctx, 0, &desc, 0) != SEC_E_OK)
                return false;
            // The trailer may come out shorter than the room made for it; the next record follows the real end.
            recordBytes = size_t(bufs[0].cbBuffer) + bufs[1].cbBuffer + bufs[2].cbBuffer;
            return true;
        }

        struct HandshakeStep
        {
            SECURITY_STATUS status;
            bool replySent;
            size_t unusedBytes; // what the client sent beyond this step
        };

        HandshakeStep AcceptStep(CredHandle &cred, std::vector<uint8_t> &in)
        {
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
            const SECURITY_STATUS ss = AcceptSecurityContext(&cred, haveCtx ? &ctx : nullptr, &inDesc,
                                                             ASC_REQ_SEQUENCE_DETECT | ASC_REQ_REPLAY_DETECT | ASC_REQ_CONFIDENTIALITY |
                                                                 ASC_REQ_EXTENDED_ERROR | ASC_REQ_ALLOCATE_MEMORY | ASC_REQ_STREAM,
                                                             0, &ctx, &outDesc, &attrs, &expiry);
            if (ss != SEC_E_INCOMPLETE_MESSAGE)
                haveCtx = true; // from here on a context exists (even when the call failed) and is freed with the state
            bool sent = true;
            if (outb[0].cbBuffer > 0 && outb[0].pvBuffer)
            {
                sent = SendAllPlain(sock, outb[0].pvBuffer, outb[0].cbBuffer);
                FreeContextBuffer(outb[0].pvBuffer);
            }
            return {ss, sent, inb[1].BufferType == SECBUFFER_EXTRA ? size_t(inb[1].cbBuffer) : 0};
        }

        // The handshake is done: bytes that came with its last step are the start of the stream.
        bool StartStream(const std::vector<uint8_t> &early, std::string *info)
        {
            if (QueryContextAttributesW(&ctx, SECPKG_ATTR_STREAM_SIZES, &sizes) != SEC_E_OK)
                return false;
            if (early.size() > enc.size())
                return false;
            memcpy(enc.data(), early.data(), early.size());
            encLen = early.size();
            SecPkgContext_ConnectionInfo ci{};
            if (info && QueryContextAttributesW(&ctx, SECPKG_ATTR_CONNECTION_INFO, &ci) == SEC_E_OK)
                *info = std::string(ci.dwProtocol & SP_PROT_TLS1_2 ? "TLS 1.2" : "TLS") + ", " + std::to_string(ci.dwCipherStrength) + "-bit";
            return true;
        }
    };

    TlsState::TlsState() : impl_(std::make_unique<Impl>()) {}
    TlsState::~TlsState() = default;

    bool TlsState::HasBuffered() const { return impl_->plainPos < impl_->plainEnd || impl_->encLen > 0; }

    bool TlsState::RecvAll(void *data, size_t len)
    {
        auto *p = static_cast<uint8_t *>(data);
        while (len > 0)
        {
            int n = impl_->ReadPlain(p, len);
            if (n <= 0)
                return false;
            p += n;
            len -= size_t(n);
        }
        return true;
    }

    bool TlsState::Send(const void *a, size_t aLen, const void *b, size_t bLen)
    {
        Impl &m = *impl_;
        const size_t total = aLen + bLen;
        if (total == 0)
            return true;
        const size_t maxMsg = m.sizes.cbMaximumMessage;
        const size_t overhead = m.sizes.cbHeader + m.sizes.cbTrailer;
        const size_t records = (total + maxMsg - 1) / maxMsg;
        const size_t need = total + records * overhead;
        if (m.out.size() < need)
            m.out.resize(need);

        const auto *pa = static_cast<const uint8_t *>(a);
        const auto *pb = static_cast<const uint8_t *>(b);
        size_t done = 0, written = 0;
        while (done < total)
        {
            const size_t n = std::min(maxMsg, total - done);
            uint8_t *rec = m.out.data() + written;
            uint8_t *data = rec + m.sizes.cbHeader;
            // Copy [done, done + n) of the concatenation of a and b into the record.
            size_t copied = 0;
            if (done < aLen)
            {
                copied = std::min(n, aLen - done);
                memcpy(data, pa + done, copied);
            }
            if (copied < n)
                memcpy(data + copied, pb + (done + copied - aLen), n - copied);

            size_t recordBytes = 0;
            if (!m.EncryptRecord(rec, n, recordBytes))
                return false;
            written += recordBytes;
            done += n;
        }
        return SendAllPlain(m.sock, m.out.data(), written);
    }

    // ---------------------------------------------------------------------------------------------
    // The host's identity.

    struct TlsIdentity::Impl
    {
        NCRYPT_PROV_HANDLE prov = 0;
        PCCERT_CONTEXT cert = nullptr;
        CredHandle cred{};
        bool haveCred = false;
        bool machine = false; // the machine-wide identity (shared with the SYSTEM pre-sign-in service), vs the per-user one
        std::string fingerprint;

        ~Impl()
        {
            if (haveCred)
                FreeCredentialsHandle(&cred);
            ForgetCertificate();
            if (prov)
                NCryptFreeObject(prov);
        }

        std::wstring FilePath() const { return machine ? MachineIdentityPath() : IdentityPath(); }
        DWORD KeyFlags() const { return machine ? NCRYPT_MACHINE_KEY_FLAG : 0; } // which key store the named key lives in

        void ForgetCertificate()
        {
            if (cert)
                CertFreeCertificateContext(cert);
            cert = nullptr;
        }

        // Schannel signs with the key the certificate points to. The key itself stays in a Windows key store
        // (per-user or machine), non-exportable, so the identity file holds only the public certificate.
        bool PointCertificateAtKey()
        {
            CRYPT_KEY_PROV_INFO kpi{};
            kpi.pwszContainerName = const_cast<LPWSTR>(kKeyName);
            kpi.pwszProvName = const_cast<LPWSTR>(MS_KEY_STORAGE_PROVIDER);
            kpi.dwFlags = KeyFlags(); // CRYPT_MACHINE_KEYSET-equivalent, so Schannel opens the machine key
            return CertSetCertificateContextProperty(cert, CERT_KEY_PROV_INFO_PROP_ID, 0, &kpi) != 0;
        }

        bool Load()
        {
            const std::vector<uint8_t> der = ReadIdentityFile(FilePath());
            if (der.empty())
                return false;
            NCRYPT_KEY_HANDLE key = 0;
            if (NCryptOpenKey(prov, &key, kKeyName, 0, KeyFlags()) != ERROR_SUCCESS)
                return false; // the file outlived its key
            cert = CertCreateCertificateContext(X509_ASN_ENCODING, der.data(), DWORD(der.size()));
            // The key under this name may have been replaced since the file was written (the file restored from a backup,
            // or deleted and made again): a certificate for another key would make every handshake fail.
            const bool same = cert && CertificateIsForKey(cert, key);
            NCryptFreeObject(key);
            return same && PointCertificateAtKey();
        }

        bool CreateAndSave()
        {
            // A new key replaces whatever an earlier identity left under this name. Creating a machine key needs
            // elevation (or SYSTEM); NCryptCreatePersistedKey fails for a standard user, and the caller falls back.
            NCRYPT_KEY_HANDLE key = 0;
            if (NCryptCreatePersistedKey(prov, &key, NCRYPT_ECDSA_P256_ALGORITHM, kKeyName, 0,
                                         NCRYPT_OVERWRITE_KEY_FLAG | KeyFlags()) != ERROR_SUCCESS)
                return false;
            if (machine)
                GrantMachineKeyToUsers(key); // before finalize, so the DACL is on the persisted key
            std::vector<uint8_t> der;
            bool ok = NCryptFinalizeKey(key, 0) == ERROR_SUCCESS && BuildSelfSignedCertificate(key, der);
            NCryptFreeObject(key);
            if (!ok)
                return false;

            // An identity that cannot be kept would change at every start and make tablets refuse the PC.
            const std::wstring path = FilePath();
            if (!WriteIdentityFile(path, der))
                return false;
            if (machine)
                RestrictMachineFile(path);
            cert = CertCreateCertificateContext(X509_ASN_ENCODING, der.data(), DWORD(der.size()));
            return cert && PointCertificateAtKey();
        }

        // Load the existing identity or make a new one, for whichever store `machine` currently selects.
        bool LoadOrMake()
        {
            if (Load())
            {
                LOGI(machine ? "tls: loaded the machine identity" : "tls: loaded the host identity");
                return true;
            }
            ForgetCertificate();
            if (!CreateAndSave())
                return false;
            LOGI(machine ? "tls: made a new machine identity" : "tls: made a new host identity");
            return true;
        }

        bool Init(bool machineWide)
        {
            if (NCryptOpenStorageProvider(&prov, MS_KEY_STORAGE_PROVIDER, 0) != ERROR_SUCCESS)
                return false;
            machine = machineWide;
            if (machine && !LoadOrMake())
            {
                // No machine identity and this process cannot create one (a standard user): keep the tablet working
                // by presenting the per-user identity. The sign-in-screen service, which runs as SYSTEM, always can.
                LOGI("tls: machine identity unavailable; falling back to the per-user identity");
                machine = false;
                ForgetCertificate();
            }
            if (!machine && !LoadOrMake())
            {
                LOGI("tls: cannot create the host identity");
                return false;
            }
            return ComputeFingerprint() && AcquireCredentials();
        }

        bool ComputeFingerprint()
        {
            uint8_t hash[32];
            DWORD hashLen = sizeof(hash);
            if (!CryptHashCertificate2(BCRYPT_SHA256_ALGORITHM, 0, nullptr, cert->pbCertEncoded, cert->cbCertEncoded, hash, &hashLen))
                return false;
            fingerprint = HexColon(hash, hashLen);
            return true;
        }

        // TLS 1.2 only: it is on every Windows 10 build we support, and TLS 1.3 would only save one round trip at connect.
        bool AcquireCredentials()
        {
            SCHANNEL_CRED sc{};
            sc.dwVersion = SCHANNEL_CRED_VERSION;
            sc.cCreds = 1;
            sc.paCred = &cert;
            sc.grbitEnabledProtocols = SP_PROT_TLS1_2_SERVER;
            sc.dwFlags = SCH_USE_STRONG_CRYPTO | SCH_CRED_NO_SYSTEM_MAPPER | SCH_CRED_NO_DEFAULT_CREDS;
            TimeStamp expiry;
            SECURITY_STATUS ss = AcquireCredentialsHandleW(nullptr, const_cast<LPWSTR>(UNISP_NAME_W), SECPKG_CRED_INBOUND, nullptr, &sc, nullptr,
                                                           nullptr, &cred, &expiry);
            if (ss != SEC_E_OK)
            {
                LOGI("tls: AcquireCredentialsHandle failed 0x%08lx", static_cast<unsigned long>(ss));
                return false;
            }
            haveCred = true;
            return true;
        }
    };

    TlsIdentity::TlsIdentity() : impl_(std::make_unique<Impl>()) {}
    TlsIdentity::~TlsIdentity() = default;

    const std::string &TlsIdentity::Fingerprint() const { return impl_->fingerprint; }

    std::unique_ptr<TlsIdentity> TlsIdentity::LoadOrCreate(bool machineWide)
    {
        std::unique_ptr<TlsIdentity> id(new TlsIdentity());
        if (!id->impl_->Init(machineWide))
            return nullptr;
        return id;
    }

    std::shared_ptr<TlsState> TlsIdentity::Accept(SOCKET s, std::string *info)
    {
        std::shared_ptr<TlsState> st(new TlsState());
        TlsState::Impl &m = *st->impl_;
        m.sock = s;

        std::vector<uint8_t> in;
        bool needData = true;
        for (;;)
        {
            if (needData)
            {
                uint8_t buf[16 * 1024];
                int got = recv(s, reinterpret_cast<char *>(buf), int(sizeof(buf)), 0);
                if (got <= 0)
                    return nullptr;
                in.insert(in.end(), buf, buf + got);
                needData = false;
                if (in.size() > kMaxHandshakeBytes)
                    return nullptr;
            }
            const TlsState::Impl::HandshakeStep step = m.AcceptStep(impl_->cred, in);
            if (!step.replySent)
                return nullptr;
            if (step.status == SEC_E_INCOMPLETE_MESSAGE)
            {
                needData = true;
                continue;
            }
            if (step.status != SEC_E_OK && step.status != SEC_I_CONTINUE_NEEDED)
            {
                LOGI("tls: handshake failed 0x%08lx", static_cast<unsigned long>(step.status));
                return nullptr;
            }
            // Bytes the client sent beyond what this step used belong to the next step (or, once done, to the stream).
            in.erase(in.begin(), in.end() - std::ptrdiff_t(step.unusedBytes));
            if (step.status == SEC_E_OK)
                return m.StartStream(in, info) ? st : nullptr;
            needData = in.empty();
        }
    }

} // namespace pd
