// PadDisplay by Nezucloud. Copyright (c) 2026 Nezucloud.
// SPDX-License-Identifier: MIT
#pragma once
#include <winsock2.h>
#include <cstddef>
#include <memory>
#include <string>

namespace pd {

// One TLS 1.2 connection (Schannel). Send is not thread-safe by itself: callers serialize it, as they
// already do for plain sockets. One thread may Send while another reads: SSPI keeps separate state for
// the two directions (.NET's SslStream relies on the same).
class TlsState {
public:
    ~TlsState();
    // Sends the bytes of a followed by those of b as one run of records and one send() call.
    bool Send(const void* a, size_t aLen, const void* b, size_t bLen);
    bool RecvAll(void* data, size_t len);
    // Decrypted or still-encrypted bytes are already waiting here, so the socket may look idle while a read would not block.
    bool HasBuffered() const;

private:
    friend class TlsIdentity;
    TlsState();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// The host's certificate and key, made on first run. The certificate is kept in %LOCALAPPDATA%\PadDisplay\tls-identity.bin;
// the private key stays in the Windows user's key store (non-exportable). Tablets remember the certificate's
// fingerprint the first time they connect.
//
// machineWide picks a single identity the whole PC shares: the cert in %ProgramData%\PadDisplay and a machine
// key store key ACL'd to SYSTEM, administrators and the signed-in user. The pre-sign-in service (SYSTEM) and
// the normal tray host then present the SAME fingerprint, so the tablet trusts the sign-in screen and the
// desktop as one PC. Creating it needs elevation (or SYSTEM); a non-elevated host that finds none falls back
// to the per-user identity. Used only when pre-sign-in access is on, so ordinary users are unaffected.
class TlsIdentity {
public:
    ~TlsIdentity();
    static std::unique_ptr<TlsIdentity> LoadOrCreate(bool machineWide = false); // null if Schannel or the key store failed
    // Server handshake on a connected socket. Blocking; the socket's SO_RCVTIMEO / SO_SNDTIMEO apply. Null = failed.
    std::shared_ptr<TlsState> Accept(SOCKET s, std::string* info = nullptr);
    const std::string& Fingerprint() const; // SHA-256 of the certificate, "AB:CD:..."

private:
    TlsIdentity();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace pd
