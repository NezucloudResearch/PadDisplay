// PadDisplay by Nezucloud. Copyright (c) 2026 Nezucloud.
// SPDX-License-Identifier: MIT
#pragma once
#include "protocol.h"

#include <winsock2.h>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace pd {

bool NetStartup();

class TlsState;

// A connected socket and, over Wi-Fi, the TLS state wrapped around it. USB (loopback) connections stay plain,
// so the tuned plain-socket code below is the same as before TLS existed.
struct Conn {
    SOCKET s = INVALID_SOCKET;
    std::shared_ptr<TlsState> tls; // null = plain
};

// Blocking helpers. Return false on error/close.
bool SendAll(Conn& c, const void* data, size_t len);
bool RecvAll(Conn& c, void* data, size_t len);
bool SendMsg(Conn& c, Msg type, const void* payload, uint32_t len);
bool RecvMsg(Conn& c, Msg& type, std::vector<uint8_t>& payload, uint32_t maxLen);
// Ends a connection politely: sends everything already queued (a Notice, say) followed by a FIN, then reads and discards
// what the peer still sends for up to drainMs. Closing straight away while the peer's data is unread would reset the
// connection, and the peer could lose the last message. The caller closes the socket afterwards.
void FinishConnection(SOCKET s, DWORD drainMs);
// Waits until a read would not block: 1 readable, 0 timed out, -1 error. With TLS, bytes that were
// already received and are waiting in the TLS layer count as readable; the socket alone would not show them.
int WaitReadable(Conn& c, long timeoutUs);
void TuneSocket(SOCKET s, bool lowLatencyVideo);
// A socket handle that a child process inherits keeps the connection (or the port) open after we close it.
void NoInherit(SOCKET s);
bool IsLoopback(const sockaddr_storage& addr);
std::string AddrToString(const sockaddr_storage& addr);

// Broadcasts "PADDISPLAY <version> <tcpPort> <hostname>" on every IPv4 interface once a second.
class DiscoveryBeacon {
public:
    void Start();
    void Stop();
private:
    void Run();
    std::atomic<bool> stop_{false};
    std::thread thread_;
};

// Keeps `adb reverse tcp:PORT tcp:PORT` applied on every attached device so the tablet can reach
// the host at 127.0.0.1 over USB.
class AdbReverser {
public:
    void Start(const std::wstring& adbPath);
    void Stop();
    int DeviceCount() const { return devices_; }
    static std::wstring FindAdb(const std::wstring& configured);
private:
    void Run();
    std::wstring adb_;
    std::atomic<bool> stop_{false};
    std::atomic<int> devices_{0};
    std::thread thread_;
};

// Runs a console program hidden and returns its stdout (empty on failure).
std::string RunCapture(const std::wstring& cmdline, DWORD timeoutMs, DWORD* exitCode = nullptr);

} // namespace pd
