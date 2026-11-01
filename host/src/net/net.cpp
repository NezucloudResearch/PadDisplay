// PadDisplay by Nezucloud. Copyright (c) 2026 Nezucloud.
// SPDX-License-Identifier: MIT
#include "net.h"
#include "tls.h"

#include <ws2tcpip.h>
#include <windows.h>
#include <mstcpip.h>
#include <algorithm>

namespace pd
{

    bool NetStartup()
    {
        WSADATA wsa;
        return WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
    }

    bool SendAllPlain(SOCKET s, const void *data, size_t len)
    {
        auto *p = static_cast<const char *>(data);
        while (len > 0)
        {
            int n = send(s, p, int(std::min<size_t>(len, 1 << 20)), 0);
            if (n <= 0)
                return false;
            p += n;
            len -= size_t(n);
        }
        return true;
    }

    static bool RecvAllPlain(SOCKET s, void *data, size_t len)
    {
        auto *p = static_cast<char *>(data);
        while (len > 0)
        {
            int n = recv(s, p, int(len), 0);
            if (n <= 0)
                return false;
            p += n;
            len -= size_t(n);
        }
        return true;
    }

    bool SendAll(Conn &c, const void *data, size_t len)
    {
        return c.tls ? c.tls->Send(data, len, nullptr, 0) : SendAllPlain(c.s, data, len);
    }

    bool RecvAll(Conn &c, void *data, size_t len)
    {
        return c.tls ? c.tls->RecvAll(data, len) : RecvAllPlain(c.s, data, len);
    }

    bool SendMsg(Conn &c, Msg type, const void *payload, uint32_t len)
    {
        uint8_t hdr[kHeaderSize];
        hdr[0] = uint8_t(type);
        wr32(hdr + 1, len);
        // TLS: Schannel encrypts in place, so header and payload are copied into its record buffer anyway and go out in one send.
        if (c.tls)
            return c.tls->Send(hdr, kHeaderSize, payload, len);
        SOCKET s = c.s;
        if (len == 0)
            return SendAllPlain(s, hdr, kHeaderSize);
        WSABUF bufs[2] = {{kHeaderSize, reinterpret_cast<char *>(hdr)},
                          {len, static_cast<char *>(const_cast<void *>(payload))}};
        DWORD sent = 0;
        if (WSASend(s, bufs, 2, &sent, 0, nullptr, nullptr) != 0)
            return false;
        if (sent == kHeaderSize + len)
            return true;
        // Partial send: finish the remainder the slow way.
        if (sent < kHeaderSize)
        {
            if (!SendAllPlain(s, hdr + sent, kHeaderSize - sent))
                return false;
            sent = kHeaderSize;
        }
        return SendAllPlain(s, static_cast<const uint8_t *>(payload) + (sent - kHeaderSize), len - (sent - kHeaderSize));
    }

    bool RecvMsg(Conn &c, Msg &type, std::vector<uint8_t> &payload, uint32_t maxLen)
    {
        uint8_t hdr[kHeaderSize];
        if (!RecvAll(c, hdr, kHeaderSize))
            return false;
        uint32_t len = rd32(hdr + 1);
        if (len > maxLen)
            return false;
        type = Msg(hdr[0]);
        payload.resize(len);
        return len == 0 || RecvAll(c, payload.data(), len);
    }

    void FinishConnection(SOCKET s, DWORD drainMs)
    {
        if (s == INVALID_SOCKET)
            return;
        shutdown(s, SD_SEND);
        DWORD timeout = drainMs;
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<char *>(&timeout), sizeof(timeout));
        char buf[2048];
        const ULONGLONG end = GetTickCount64() + drainMs;
        while (GetTickCount64() < end && recv(s, buf, sizeof(buf), 0) > 0)
        {
        }
    }

    void CutOff(SOCKET s)
    {
        shutdown(s, SD_BOTH);
        CancelIoEx(reinterpret_cast<HANDLE>(s), nullptr);
    }

    int WaitReadable(Conn &c, long timeoutUs)
    {
        if (c.tls && c.tls->HasBuffered())
            return 1;
        fd_set rs;
        FD_ZERO(&rs);
        FD_SET(c.s, &rs);
        timeval tv{0, timeoutUs};
        return select(0, &rs, nullptr, nullptr, &tv);
    }

    void NoInherit(SOCKET s) { SetHandleInformation(reinterpret_cast<HANDLE>(s), HANDLE_FLAG_INHERIT, 0); }

    void TuneSocket(SOCKET s, bool lowLatencyVideo)
    {
        BOOL one = TRUE;
        setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<char *>(&one), sizeof(one));
        // A modest send buffer keeps queueing delay bounded; RTT then reflects congestion quickly.
        int sndbuf = lowLatencyVideo ? 512 * 1024 : 64 * 1024;
        setsockopt(s, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<char *>(&sndbuf), sizeof(sndbuf));
        tcp_keepalive ka{1, 5000, 1000};
        DWORD ret = 0;
        WSAIoctl(s, SIO_KEEPALIVE_VALS, &ka, sizeof(ka), nullptr, 0, &ret, nullptr, nullptr);
    }

    bool IsLoopback(const sockaddr_storage &addr)
    {
        if (addr.ss_family == AF_INET)
            return (ntohl(reinterpret_cast<const sockaddr_in &>(addr).sin_addr.s_addr) >> 24) == 127;
        if (addr.ss_family == AF_INET6)
        {
            auto &a6 = reinterpret_cast<const sockaddr_in6 &>(addr).sin6_addr;
            if (IN6_IS_ADDR_LOOPBACK(&a6))
                return true;
            if (IN6_IS_ADDR_V4MAPPED(&a6))
                return a6.u.Byte[12] == 127;
        }
        return false;
    }

    std::string AddrToString(const sockaddr_storage &addr)
    {
        char buf[INET6_ADDRSTRLEN] = "?";
        if (addr.ss_family == AF_INET)
            inet_ntop(AF_INET, &reinterpret_cast<const sockaddr_in &>(addr).sin_addr, buf, sizeof(buf));
        else if (addr.ss_family == AF_INET6)
            inet_ntop(AF_INET6, &reinterpret_cast<const sockaddr_in6 &>(addr).sin6_addr, buf, sizeof(buf));
        return buf;
    }

} // namespace pd
