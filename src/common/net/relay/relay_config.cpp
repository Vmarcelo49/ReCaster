// src/common/net/relay/relay_config.cpp

#include "relay_config.hpp"

#include <algorithm>
#include <cstdlib>
#include <string>
#include <string_view>

namespace caster::common::net::relay_config {

const char* kDefaultRelayList =
    "# Default relay servers (one per line, format host[:port]).\n"
    "# Port defaults to 3939 if omitted.\n"
    "zzcaster.duckdns.org:3939\n";

namespace {

std::string trim(std::string_view s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r')) ++a;
    while (b > a && (s[b-1] == ' ' || s[b-1] == '\t' || s[b-1] == '\r')) --b;
    return std::string(s.substr(a, b - a));
}

} // namespace

std::vector<RelayEntry>::value_type parse_line(std::string line) {
    RelayEntry out;
    out.port = kDefaultRelayPort;

    std::string t = trim(line);
    if (t.empty() || t[0] == '#' || t[0] == ';') {
        out.host.clear();  // signal "skip"
        return out;
    }

    // Find the LAST ':' (handles IPv6, though we don't support it here).
    auto colon = t.rfind(':');
    if (colon != std::string::npos) {
        std::string port_str = t.substr(colon + 1);
        // Try to parse as a port number.
        try {
            int p = std::stoi(port_str);
            if (p >= 1 && p <= 65535) {
                out.host = t.substr(0, colon);
                out.port = static_cast<std::uint16_t>(p);
                return out;
            }
        } catch (...) {
            // Not a number — treat the whole line as a hostname.
        }
    }

    out.host = t;
    return out;
}

RelayList parse_list(const std::string& content) {
    RelayList list;
    std::string current;
    for (char c : content) {
        if (c == '\n') {
            RelayEntry e = parse_line(current);
            if (!e.host.empty()) list.push_back(e);
            current.clear();
        } else {
            current += c;
        }
    }
    if (!current.empty()) {
        RelayEntry e = parse_line(current);
        if (!e.host.empty()) list.push_back(e);
    }
    return list;
}

RelayList default_list() {
    return parse_list(kDefaultRelayList);
}

} // namespace caster::common::net::relay_config
