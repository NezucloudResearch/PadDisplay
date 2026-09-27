// PadDisplay by Nezucloud. Copyright (c) 2026 Nezucloud.
// SPDX-License-Identifier: MIT
#pragma once
#include "protocol.h"

#include <winsock2.h>
#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

namespace pd {

bool NetStartup();

// Blocking helpers. Return false on error/close.
bool SendAll(SOCKET s, const void* data, size_t len);
bool RecvAll(SOCKET s, void* data, size_t len);
bool SendMsg(SOCKET s, Msg type, const void* payload, uint32_t len);
bool RecvMsg(SOCKET s, Msg& type, std::vector<uint8_t>& payload, uint32_t maxLen);
void TuneSocket(SOCKET s, bool lowLatencyVideo);
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
