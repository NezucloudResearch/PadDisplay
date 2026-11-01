// PadDisplay by Nezucloud. Copyright (c) 2026 Nezucloud.
// SPDX-License-Identifier: MIT
#pragma once
#include "protocol.h"

#include <winsock2.h>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace pd
{

    bool NetStartup();

    class TlsState;

    // A connected socket and, over Wi-Fi, the TLS state wrapped around it. USB (loopback) connections stay plain,
    // so the tuned plain-socket code below is the same as before TLS existed.
    struct Conn
    {
        SOCKET s = INVALID_SOCKET;
        std::shared_ptr<TlsState> tls; // null = plain
    };

    // Blocking helpers. Return false on error/close.
    bool SendAll(Conn &c, const void *data, size_t len);
    bool RecvAll(Conn &c, void *data, size_t len);
    bool SendAllPlain(SOCKET s, const void *data, size_t len); // straight to the socket, past any TLS
    bool SendMsg(Conn &c, Msg type, const void *payload, uint32_t len);
    bool RecvMsg(Conn &c, Msg &type, std::vector<uint8_t> &payload, uint32_t maxLen);
    // Ends a connection politely: sends everything already queued (a Notice, say) followed by a FIN, then reads and discards
    // what the peer still sends for up to drainMs. Closing straight away while the peer's data is unread would reset the
    // connection, and the peer could lose the last message. The caller closes the socket afterwards.
    void FinishConnection(SOCKET s, DWORD drainMs);
    // Ends a connection from another thread. shutdown() alone does not wake a recv() that is already waiting (it would sit
    // until the peer closes or the timeout runs out); cancelling the socket's I/O does. The socket stays open: its own thread closes it.
    void CutOff(SOCKET s);
    // Waits until a read would not block: 1 readable, 0 timed out, -1 error. With TLS, bytes that were
    // already received and are waiting in the TLS layer count as readable; the socket alone would not show them.
    int WaitReadable(Conn &c, long timeoutUs);
    void TuneSocket(SOCKET s, bool lowLatencyVideo);
    // A socket handle that a child process inherits keeps the connection (or the port) open after we close it.
    void NoInherit(SOCKET s);
    bool IsLoopback(const sockaddr_storage &addr);
    std::string AddrToString(const sockaddr_storage &addr);

} // namespace pd
