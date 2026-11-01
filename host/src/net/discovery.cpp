// PadDisplay by Nezucloud. Copyright (c) 2026 Nezucloud.
// SPDX-License-Identifier: MIT
#include "discovery.h"
#include "protocol.h"

#include <winsock2.h>
#include <iphlpapi.h>
#include <cstdint>
#include <string>
#include <vector>

namespace pd
{
    namespace
    {

        constexpr DWORD kBeaconIntervalMs = 1000;
        constexpr int kBeaconsPerAddressRefresh = 10; // networks come and go

        std::vector<uint32_t> BroadcastAddresses()
        {
            std::vector<uint32_t> result{INADDR_BROADCAST};
            ULONG size = 16 * 1024;
            std::vector<uint8_t> buf(size);
            auto *aa = reinterpret_cast<IP_ADAPTER_ADDRESSES *>(buf.data());
            ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
            if (GetAdaptersAddresses(AF_INET, flags, nullptr, aa, &size) == ERROR_BUFFER_OVERFLOW)
            {
                buf.resize(size);
                aa = reinterpret_cast<IP_ADAPTER_ADDRESSES *>(buf.data());
                if (GetAdaptersAddresses(AF_INET, flags, nullptr, aa, &size) != NO_ERROR)
                    return result;
            }
            for (auto *a = aa; a; a = a->Next)
            {
                if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK)
                    continue;
                for (auto *u = a->FirstUnicastAddress; u; u = u->Next)
                {
                    uint32_t ip = ntohl(reinterpret_cast<sockaddr_in *>(u->Address.lpSockaddr)->sin_addr.s_addr);
                    uint8_t prefix = u->OnLinkPrefixLength;
                    if (prefix == 0 || prefix >= 31)
                        continue;
                    uint32_t mask = 0xFFFFFFFFu << (32 - prefix);
                    result.push_back(htonl(ip | ~mask));
                }
            }
            return result;
        }

        std::string BeaconText()
        {
            char host[256] = "PC";
            DWORD hostLen = sizeof(host);
            GetComputerNameA(host, &hostLen);
            return "PADDISPLAY " + std::to_string(kProtocolVersion) + " " + std::to_string(kTcpPort) + " " + host;
        }

    } // namespace

    void DiscoveryBeacon::Start()
    {
        stop_.Reset();
        thread_ = std::thread([this]
                              { Run(); });
    }

    void DiscoveryBeacon::Stop()
    {
        stop_.Request();
        if (thread_.joinable())
            thread_.join();
    }

    void DiscoveryBeacon::Run()
    {
        SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (s == INVALID_SOCKET)
            return;
        BOOL one = TRUE;
        setsockopt(s, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<char *>(&one), sizeof(one));
        const std::string text = BeaconText();
        int beacons = 0;
        std::vector<uint32_t> targets;
        do
        {
            if (beacons++ % kBeaconsPerAddressRefresh == 0)
                targets = BroadcastAddresses();
            for (uint32_t addr : targets)
            {
                sockaddr_in to{};
                to.sin_family = AF_INET;
                to.sin_port = htons(kDiscoveryPort);
                to.sin_addr.s_addr = addr;
                sendto(s, text.data(), int(text.size()), 0, reinterpret_cast<sockaddr *>(&to), sizeof(to));
            }
        } while (!stop_.WaitFor(kBeaconIntervalMs));
        closesocket(s);
    }

} // namespace pd
