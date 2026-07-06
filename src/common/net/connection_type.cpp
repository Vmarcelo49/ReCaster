// src/common/net/connection_type.cpp

#include "connection_type.hpp"
#include "../logger.hpp"

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

#include <cstring>
#include <string>
#include <vector>

namespace caster::common::net::connection_type {

namespace {

// Detect Wine by looking for wine_get_version in ntdll.dll.
bool is_wine() {
    HMODULE ntdll = GetModuleHandleA("ntdll.dll");
    if (!ntdll) return false;
    return GetProcAddress(ntdll, "wine_get_version") != nullptr;
}

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
std::string get_windows_connection_type() {
    constexpr ULONG kFlags = GAA_FLAG_SKIP_ANYCAST |
                              GAA_FLAG_SKIP_MULTICAST |
                              GAA_FLAG_SKIP_DNS_SERVER;

    ULONG buf_len = 0;
    ULONG rc = GetAdaptersAddresses(AF_UNSPEC, kFlags, nullptr, nullptr,
                                     &buf_len);
    if (rc == ERROR_BUFFER_OVERFLOW && buf_len > 0) {
        // Allocate the buffer and try again.
        std::vector<std::uint8_t> buf(buf_len);
        rc = GetAdaptersAddresses(AF_UNSPEC, kFlags, nullptr,
                                   reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()),
                                   &buf_len);
        if (rc != ERROR_SUCCESS) {
            return "Unknown";
        }

        bool has_wifi = false;
        bool has_ethernet = false;
        for (auto* a = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
             a; a = a->Next) {
            // Skip loopback (IfType 24).
            if (a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
            if (a->IfType == IF_TYPE_IEEE80211) {
                has_wifi = true;
            } else if (a->IfType == IF_TYPE_ETHERNET_CSMACD) {
                has_ethernet = true;
            }
        }
        if (has_wifi) return "Wireless";
        if (has_ethernet) return "Wired";
    }
    return "Unknown";
}

} // namespace

std::string get_connection_type() {
    if (is_wine()) {
        return get_linux_connection_type();
    }
    return get_windows_connection_type();
}

} // namespace caster::common::net::connection_type
