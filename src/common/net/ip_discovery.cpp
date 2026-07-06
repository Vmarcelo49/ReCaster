// src/common/net/ip_discovery.cpp

#include "ip_discovery.hpp"
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
#include <wininet.h>

#include <cstdio>
#include <cstring>
#include <string>

// Link wininet.lib — on MinGW we do this via CMake target_link_libraries.
// On MSVC you'd use #pragma comment(lib, "wininet.lib").

namespace caster::common::net::ip_discovery {

std::string get_public_ip() {
    HINTERNET session = InternetOpenA("recaster", INTERNET_OPEN_TYPE_PRECONFIG,
                                       nullptr, nullptr, 0);
    if (!session) {
        logger::warn("ip_discovery: InternetOpenA failed (err={})",
                     GetLastError());
        return {};
    }

    HINTERNET request = InternetOpenUrlA(
        session,
        "https://api.ipify.org",
        nullptr, 0,
        INTERNET_FLAG_RELOAD, 0);
    if (!request) {
        logger::warn("ip_discovery: InternetOpenUrlA failed (err={})",
                     GetLastError());
        InternetCloseHandle(session);
        return {};
    }

    char buf[64] = {0};
    DWORD read = 0;
    BOOL ok = InternetReadFile(request, buf, sizeof(buf) - 1, &read);
    InternetCloseHandle(request);
    InternetCloseHandle(session);

    if (!ok || read == 0) {
        logger::warn("ip_discovery: InternetReadFile failed or empty");
        return {};
    }
    buf[read] = '\0';

    // Strip trailing whitespace (\n, \r, space, tab).
    while (read > 0) {
        char c = buf[read - 1];
        if (c == '\n' || c == '\r' || c == ' ' || c == '\t') {
            buf[--read] = '\0';
        } else {
            break;
        }
    }
    return std::string(buf, read);
}

std::string get_local_ip() {
    char hostname[256] = {0};
    if (gethostname(hostname, sizeof(hostname) - 1) != 0) {
        logger::warn("ip_discovery: gethostname failed (err={})",
                     WSAGetLastError());
        return {};
    }
    hostname[sizeof(hostname) - 1] = '\0';

    struct hostent* he = gethostbyname(hostname);
    if (!he || he->h_addr_list[0] == nullptr) {
        logger::warn("ip_discovery: gethostbyname('{}') failed", hostname);
        return {};
    }

    // h_addr_list[0] is a pointer to a 4-byte in_addr in network byte order.
    const unsigned char* bytes =
        reinterpret_cast<const unsigned char*>(he->h_addr_list[0]);
    char ip[16];
    std::snprintf(ip, sizeof(ip), "%u.%u.%u.%u",
                  bytes[0], bytes[1], bytes[2], bytes[3]);
    return std::string(ip);
}

} // namespace caster::common::net::ip_discovery
