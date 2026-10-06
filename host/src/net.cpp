// PadDisplay by Nezucloud. Copyright (c) 2026 Nezucloud.
// SPDX-License-Identifier: MIT
#include "net.h"
#include "log.h"

#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <windows.h>
#include <mstcpip.h>
#include <map>
#include <set>
#include <sstream>

namespace pd {

bool NetStartup() {
    WSADATA wsa;
    return WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
}

bool SendAll(SOCKET s, const void* data, size_t len) {
    auto* p = static_cast<const char*>(data);
    while (len > 0) {
        int n = send(s, p, int(std::min<size_t>(len, 1 << 20)), 0);
        if (n <= 0) return false;
        p += n;
        len -= size_t(n);
    }
    return true;
}

bool RecvAll(SOCKET s, void* data, size_t len) {
    auto* p = static_cast<char*>(data);
    while (len > 0) {
        int n = recv(s, p, int(len), 0);
        if (n <= 0) return false;
        p += n;
        len -= size_t(n);
    }
    return true;
}

bool SendMsg(SOCKET s, Msg type, const void* payload, uint32_t len) {
    uint8_t hdr[kHeaderSize];
    hdr[0] = uint8_t(type);
    wr32(hdr + 1, len);
    if (len == 0) return SendAll(s, hdr, kHeaderSize);
    WSABUF bufs[2] = {{kHeaderSize, reinterpret_cast<char*>(hdr)},
                      {len, static_cast<char*>(const_cast<void*>(payload))}};
    DWORD sent = 0;
    if (WSASend(s, bufs, 2, &sent, 0, nullptr, nullptr) != 0) return false;
    if (sent == kHeaderSize + len) return true;
    // Partial send: finish the remainder the slow way.
    if (sent < kHeaderSize) {
        if (!SendAll(s, hdr + sent, kHeaderSize - sent)) return false;
        sent = kHeaderSize;
    }
    return SendAll(s, static_cast<const uint8_t*>(payload) + (sent - kHeaderSize), len - (sent - kHeaderSize));
}

bool RecvMsg(SOCKET s, Msg& type, std::vector<uint8_t>& payload, uint32_t maxLen) {
    uint8_t hdr[kHeaderSize];
    if (!RecvAll(s, hdr, kHeaderSize)) return false;
    uint32_t len = rd32(hdr + 1);
    if (len > maxLen) return false;
    type = Msg(hdr[0]);
    payload.resize(len);
    return len == 0 || RecvAll(s, payload.data(), len);
}

void NoInherit(SOCKET s) { SetHandleInformation(reinterpret_cast<HANDLE>(s), HANDLE_FLAG_INHERIT, 0); }

void TuneSocket(SOCKET s, bool lowLatencyVideo) {
    BOOL one = TRUE;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<char*>(&one), sizeof(one));
    // A modest send buffer keeps queueing delay bounded; RTT then reflects congestion quickly.
    int sndbuf = lowLatencyVideo ? 512 * 1024 : 64 * 1024;
    setsockopt(s, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<char*>(&sndbuf), sizeof(sndbuf));
    tcp_keepalive ka{1, 5000, 1000};
    DWORD ret = 0;
    WSAIoctl(s, SIO_KEEPALIVE_VALS, &ka, sizeof(ka), nullptr, 0, &ret, nullptr, nullptr);
}

bool IsLoopback(const sockaddr_storage& addr) {
    if (addr.ss_family == AF_INET)
        return (ntohl(reinterpret_cast<const sockaddr_in&>(addr).sin_addr.s_addr) >> 24) == 127;
    if (addr.ss_family == AF_INET6) {
        auto& a6 = reinterpret_cast<const sockaddr_in6&>(addr).sin6_addr;
        if (IN6_IS_ADDR_LOOPBACK(&a6)) return true;
        if (IN6_IS_ADDR_V4MAPPED(&a6)) return a6.u.Byte[12] == 127;
    }
    return false;
}

std::string AddrToString(const sockaddr_storage& addr) {
    char buf[INET6_ADDRSTRLEN] = "?";
    if (addr.ss_family == AF_INET)
        inet_ntop(AF_INET, &reinterpret_cast<const sockaddr_in&>(addr).sin_addr, buf, sizeof(buf));
    else if (addr.ss_family == AF_INET6)
        inet_ntop(AF_INET6, &reinterpret_cast<const sockaddr_in6&>(addr).sin6_addr, buf, sizeof(buf));
    return buf;
}

// ---------------------------------------------------------------------------------------------

void DiscoveryBeacon::Start() {
    stop_ = false;
    thread_ = std::thread([this] { Run(); });
}

void DiscoveryBeacon::Stop() {
    stop_ = true;
    if (thread_.joinable()) thread_.join();
}

static std::vector<uint32_t> BroadcastAddresses() {
    std::vector<uint32_t> result{INADDR_BROADCAST};
    ULONG size = 16 * 1024;
    std::vector<uint8_t> buf(size);
    auto* aa = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
    ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    if (GetAdaptersAddresses(AF_INET, flags, nullptr, aa, &size) == ERROR_BUFFER_OVERFLOW) {
        buf.resize(size);
        aa = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
        if (GetAdaptersAddresses(AF_INET, flags, nullptr, aa, &size) != NO_ERROR) return result;
    }
    for (auto* a = aa; a; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
            uint32_t ip = ntohl(reinterpret_cast<sockaddr_in*>(u->Address.lpSockaddr)->sin_addr.s_addr);
            uint8_t prefix = u->OnLinkPrefixLength;
            if (prefix == 0 || prefix >= 31) continue;
            uint32_t mask = prefix == 0 ? 0 : 0xFFFFFFFFu << (32 - prefix);
            result.push_back(htonl(ip | ~mask));
        }
    }
    return result;
}

void DiscoveryBeacon::Run() {
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return;
    BOOL one = TRUE;
    setsockopt(s, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<char*>(&one), sizeof(one));
    char host[256] = "PC";
    DWORD hostLen = sizeof(host);
    GetComputerNameA(host, &hostLen);
    std::string msg = "PADDISPLAY " + std::to_string(kProtocolVersion) + " " + std::to_string(kTcpPort) + " " + host;

    int tick = 0;
    std::vector<uint32_t> targets;
    while (!stop_) {
        if (tick++ % 10 == 0) targets = BroadcastAddresses(); // refresh when networks change
        for (uint32_t addr : targets) {
            sockaddr_in to{};
            to.sin_family = AF_INET;
            to.sin_port = htons(kDiscoveryPort);
            to.sin_addr.s_addr = addr;
            sendto(s, msg.data(), int(msg.size()), 0, reinterpret_cast<sockaddr*>(&to), sizeof(to));
        }
        for (int i = 0; i < 10 && !stop_; ++i) Sleep(100);
    }
    closesocket(s);
}

// ---------------------------------------------------------------------------------------------

std::string RunCapture(const std::wstring& cmdline, DWORD timeoutMs, DWORD* exitCode) {
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return {};
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    // Only the pipe's write end is inherited. Inheriting every inheritable handle leaked the listening
    // socket into the adb server that `adb devices` starts and leaves running: after Exit the port stayed
    // bound, and the accept thread could not be woken by closing the socket, so Exit hung.
    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof(si);
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdOutput = si.StartupInfo.hStdError = wr;
    SIZE_T attrSize = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attrSize);
    std::vector<uint8_t> attrBuf(attrSize);
    si.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attrBuf.data());
    bool listOk = InitializeProcThreadAttributeList(si.lpAttributeList, 1, 0, &attrSize) &&
                  UpdateProcThreadAttribute(si.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, &wr, sizeof(wr), nullptr, nullptr);
    PROCESS_INFORMATION pi{};
    std::wstring cmd = cmdline;
    BOOL ok = listOk && CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT,
                                       nullptr, nullptr, &si.StartupInfo, &pi);
    if (listOk) DeleteProcThreadAttributeList(si.lpAttributeList);
    CloseHandle(wr);
    std::string out;
    if (ok) {
        char buf[4096];
        DWORD n = 0;
        ULONGLONG deadline = GetTickCount64() + timeoutMs;
        // Poll instead of reading to EOF: a spawned adb server daemon inherits the pipe and keeps it open.
        for (;;) {
            DWORD avail = 0;
            if (!PeekNamedPipe(rd, nullptr, 0, nullptr, &avail, nullptr)) break; // writer closed
            if (avail > 0) {
                if (!ReadFile(rd, buf, std::min<DWORD>(avail, sizeof(buf)), &n, nullptr) || n == 0) break;
                out.append(buf, n);
                continue;
            }
            if (WaitForSingleObject(pi.hProcess, 20) == WAIT_OBJECT_0) {
                if (PeekNamedPipe(rd, nullptr, 0, nullptr, &avail, nullptr) && avail > 0) continue;
                break;
            }
            if (GetTickCount64() > deadline) {
                TerminateProcess(pi.hProcess, 1);
                break;
            }
        }
        WaitForSingleObject(pi.hProcess, 1000);
        if (exitCode) GetExitCodeProcess(pi.hProcess, exitCode);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
    CloseHandle(rd);
    return out;
}

std::wstring AdbReverser::FindAdb(const std::wstring& configured) {
    std::vector<std::wstring> candidates;
    if (!configured.empty()) candidates.push_back(configured);
    candidates.push_back(L"C:\\android-platform-tools\\adb.exe");
    wchar_t local[MAX_PATH];
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH)) candidates.push_back(std::wstring(local) + L"\\Android\\Sdk\\platform-tools\\adb.exe");
    wchar_t found[MAX_PATH];
    if (SearchPathW(nullptr, L"adb.exe", nullptr, MAX_PATH, found, nullptr)) candidates.push_back(found);
    for (auto& c : candidates)
        if (GetFileAttributesW(c.c_str()) != INVALID_FILE_ATTRIBUTES) return c;
    return {};
}

void AdbReverser::Start(const std::wstring& adbPath) {
    adb_ = FindAdb(adbPath);
    if (adb_.empty()) {
        LOGI("adb: not found; USB mode disabled (set adbPath in settings.ini)");
        return;
    }
    LOGI("adb: using %ls", adb_.c_str());
    stop_ = false;
    thread_ = std::thread([this] { Run(); });
}

void AdbReverser::Stop() {
    stop_ = true;
    if (thread_.joinable()) thread_.join();
}

void AdbReverser::Run() {
    const std::wstring q = L"\"" + adb_ + L"\"";
    const std::wstring spec = L"tcp:" + std::to_wstring(kTcpPort);
    std::map<std::string, ULONGLONG> reversed; // serial -> when the reverse was last applied
    while (!stop_) {
        std::istringstream lines(RunCapture(q + L" devices", 10000));
        std::set<std::string> present;
        std::string line;
        while (std::getline(lines, line)) {
            auto tab = line.find('\t');
            if (tab == std::string::npos) continue;
            std::string serial = line.substr(0, tab), state = line.substr(tab + 1);
            while (!state.empty() && (state.back() == '\r' || state.back() == ' ')) state.pop_back();
            if (state == "device") present.insert(serial);
            else if (state == "unauthorized") LOGI("adb: %s is unauthorized - accept the USB debugging prompt on the tablet", serial.c_str());
        }
        for (auto it = reversed.begin(); it != reversed.end();)
            it = present.count(it->first) ? std::next(it) : reversed.erase(it);
        ULONGLONG now = GetTickCount64();
        for (auto& serial : present) {
            // Re-apply every 30 s: another adb client (e.g. scrcpy) may have restarted the server.
            auto it = reversed.find(serial);
            if (it != reversed.end() && now - it->second < 30000) continue;
            DWORD code = 1;
            RunCapture(q + L" -s " + Widen(serial) + L" reverse " + spec + L" " + spec, 10000, &code);
            if (it == reversed.end() || code != 0) LOGI("adb: reverse %s %s", serial.c_str(), code == 0 ? "ok" : "failed");
            if (code == 0) reversed[serial] = now;
        }
        devices_ = int(present.size());
        for (int i = 0; i < 20 && !stop_; ++i) Sleep(100);
    }
}

} // namespace pd
