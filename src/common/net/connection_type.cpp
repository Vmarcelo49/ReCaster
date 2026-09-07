// src/common/net/connection_type.cpp

#include "connection_type.hpp"
#include "../logger.hpp"
#include "../win32/env.hpp"

#ifndef NOMINMAX
#  define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <netioapi.h>

#include <cstring>
#include <string>
#include <vector>

namespace caster::common::net::connection_type {

namespace {

// Wine fallback: read /proc/net/route (via Z:\ path translation),
// find the default-route iface, check if /sys/class/net/<iface>/wireless
// exists. Returns "Wireless" / "Wired" / "Unknown".
std::string get_linux_connection_type() {
    // Open Z:\proc\net\route
    HANDLE f = CreateFileA("Z:\\proc\\net\\route",
                            GENERIC_READ, FILE_SHARE_READ, nullptr,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        return "Unknown";
    }

    char buf[8192] = {0};
    DWORD read = 0;
    ReadFile(f, buf, sizeof(buf) - 1, &read, nullptr);
    CloseHandle(f);
    buf[read] = '\0';

    // Find the line where destination == "00000000" (default route).
    // Format: Iface Destination Gateway Flags ... Mask MTU Window IRTT
    char iface[64] = {0};
    const char* line = buf;
    const char* end = buf + read;
    while (line < end) {
        const char* nl = (const char*)memchr(line, '\n', end - line);
        if (!nl) nl = end;
        size_t linelen = nl - line;

        // Skip header line (which contains "Iface" at start).
        if (linelen > 0 && line[0] != 'I') {
            // Parse first whitespace-delimited token as iface name.
            size_t i = 0;
            while (i < linelen && line[i] != ' ' && line[i] != '\t') {
                if (i < sizeof(iface) - 1) iface[i] = line[i];
                ++i;
            }
            iface[i] = '\0';

            // After iface, skip whitespace, then look for "00000000" dest.
            while (i < linelen && (line[i] == ' ' || line[i] == '\t')) ++i;
            if (i + 8 <= linelen &&
                std::memcmp(line + i, "00000000", 8) == 0 &&
                iface[0] != '\0') {
                // Found default route. Check if iface is wireless.
                char wireless_path[256];
                std::snprintf(wireless_path, sizeof(wireless_path),
                              "Z:\\sys\\class\\net\\%s\\wireless", iface);
                DWORD attrs = GetFileAttributesA(wireless_path);
                if (attrs != INVALID_FILE_ATTRIBUTES &&
                    !(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
                    return "Wireless";
                }
                return "Wired";
            }
        }

        line = nl + 1;
    }
    return "Unknown";
}

// Native Windows: enumerate adapters via GetAdaptersAddresses.
//
// Two rules that the old code got wrong:
//   1. OperStatus is checked — a disconnected WiFi card still has
//      IfType == IEEE80211, and the old code reported "Wireless" for a
//      machine whose traffic was 100% on Ethernet.
//   2. The adapter carrying the default IPv4 route wins. On a laptop
//      with cable + WiFi both up, the old code always said Wireless;
//      now we classify whichever interface actually forwards traffic
//      (GetIpForwardTable2, lowest-metric 0.0.0.0/0).
std::string get_windows_connection_type() {
    constexpr ULONG kFlags = GAA_FLAG_SKIP_ANYCAST |
                              GAA_FLAG_SKIP_MULTICAST |
                              GAA_FLAG_SKIP_DNS_SERVER;

    ULONG buf_len = 0;
    ULONG rc = GetAdaptersAddresses(AF_UNSPEC, kFlags, nullptr, nullptr,
                                     &buf_len);
    if (rc != ERROR_BUFFER_OVERFLOW || buf_len == 0) {
        return "Unknown";
    }
    std::vector<std::uint8_t> buf(buf_len);
    rc = GetAdaptersAddresses(AF_UNSPEC, kFlags, nullptr,
                               reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()),
                               &buf_len);
    if (rc != ERROR_SUCCESS) {
        return "Unknown";
    }

    struct UsableAdapter {
        ULONG  if_index;
        IFTYPE if_type;
    };
    std::vector<UsableAdapter> usable;
    for (auto* a = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
         a; a = a->Next) {
        // Loopback never carries game traffic.
        if (a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        // VPN / virtual-switch ports are not the physical link.
        if (a->TunnelType != TUNNEL_TYPE_NONE) continue;
        // Disconnected / disabled / dormant adapters have no traffic.
        if (a->OperStatus != IfOperStatusUp) continue;
        usable.push_back({a->IfIndex, a->IfType});
    }
    if (usable.empty()) {
        return "Unknown";
    }

    auto classify = [](IFTYPE t) -> const char* {
        switch (t) {
            case IF_TYPE_IEEE80211:
                return "Wireless";
            case IF_TYPE_ETHERNET_CSMACD:
                return "Wired";
            // Cellular data behaves like (or worse than) WiFi for
            // netplay purposes — warn rather than claim wired.
            case IF_TYPE_WWANPP:
            case IF_TYPE_WWANPP2:
                return "Wireless";
            default:
                return nullptr;
        }
    };

    // Preferred: classify the interface that owns the default IPv4
    // route (lowest metric wins — that is where game packets go).
    PMIB_IPFORWARD_TABLE2 routes = nullptr;
    if (GetIpForwardTable2(AF_INET, &routes) == NO_ERROR && routes) {
        ULONG best_metric = 0xFFFFFFFF;
        ULONG best_iface  = 0;
        for (ULONG i = 0; i < routes->NumEntries; ++i) {
            const MIB_IPFORWARD_ROW2& row = routes->Table[i];
            if (row.DestinationPrefix.PrefixLength != 0) continue;
            if (row.DestinationPrefix.Prefix.Ipv4.sin_family != AF_INET) {
                continue;
            }
            if (row.Metric < best_metric) {
                best_metric = row.Metric;
                best_iface  = row.InterfaceIndex;
            }
        }
        FreeMibTable(routes);
        if (best_iface != 0) {
            for (const auto& u : usable) {
                if (u.if_index == best_iface) {
                    if (const char* label = classify(u.if_type)) {
                        return label;
                    }
                    return "Unknown";
                }
            }
            // Default route points at an adapter we filtered out
            // (down, tunnel, loopback) — fall through to the
            // up-adapter fallback below.
        }
    }

    // Fallback (no readable route table): an up Ethernet link beats an
    // up WiFi link. Deliberately the reverse of the old priority — a
    // wired machine with an idle WiFi card must read "Wired".
    bool has_wifi = false;
    bool has_ethernet = false;
    for (const auto& u : usable) {
        const char* label = classify(u.if_type);
        if (!label) continue;
        if (std::strcmp(label, "Wireless") == 0) {
            has_wifi = true;
        } else {
            has_ethernet = true;
        }
    }
    if (has_ethernet) return "Wired";
    if (has_wifi) return "Wireless";
    return "Unknown";
}

} // namespace

std::string get_connection_type() {
    if (win32::env::is_wine()) {
        return get_linux_connection_type();
    }
    return get_windows_connection_type();
}

} // namespace caster::common::net::connection_type
