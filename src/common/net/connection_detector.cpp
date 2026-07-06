// src/common/net/connection_detector.cpp

#include "connection_detector.hpp"

#include <cctype>
#include <string>
#include <string_view>

namespace caster::common::net::connection_detector {

namespace {

// Check if `s` is a valid port number (1..65535, digits only).
// Returns the parsed port in `out_port` on success.
bool parse_port(std::string_view s, int& out_port) {
    if (s.empty() || s.size() > 5) return false;
    int v = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
        v = v * 10 + (c - '0');
        if (v > 65535) return false;
    }
    if (v < 1) return false;
    out_port = v;
    return true;
}

// Check if `s` is a valid IPv4 address (dotted quad, each octet 0..255).
// Also accepts hostnames (anything non-empty without ':' and without '#').
// We're permissive here — actual DNS resolution happens at connect time.
bool looks_like_host(std::string_view s) {
    if (s.empty()) return false;
    // Reject if it contains characters that aren't valid in hostnames.
    for (unsigned char c : s) {
        if (c == ':' || c == '#' || c == '/' || c == ' ' || c == '\t') {
            return false;
        }
        // Allow letters, digits, dots, dashes, underscores (the last is
        // technically not RFC-valid but common in LAN hostnames).
        if (!(std::isalnum(c) || c == '.' || c == '-' || c == '_')) {
            return false;
        }
    }
    return true;
}

// Check if `s` is a valid 4-letter room code (uppercase A-Z only).
bool parse_room_code(std::string_view s, std::string& out_code) {
    if (s.size() != 4) return false;
    for (char c : s) {
        if (c < 'A' || c > 'Z') return false;
    }
    out_code = std::string(s);
    return true;
}

} // namespace

ParseResult parse_input(const std::string& input) {
    ParseResult r;

    // Trim leading/trailing whitespace.
    size_t start = 0;
    size_t end = input.size();
    while (start < end && (input[start] == ' ' || input[start] == '\t')) ++start;
    while (end > start && (input[end-1] == ' ' || input[end-1] == '\t')) --end;
    std::string trimmed = input.substr(start, end - start);

    // 1. Empty → smart host with random port.
    if (trimmed.empty()) {
        r.type = InputType::Empty;
        return r;
    }

    // 2. Starts with '#' → room code.
    if (trimmed[0] == '#') {
        std::string code;
        if (parse_room_code(trimmed.substr(1), code)) {
            r.type = InputType::RoomCode;
            r.room_code = code;
            return r;
        }
        r.type = InputType::Invalid;
        r.error = "Room code must be exactly 4 uppercase letters (e.g. #ABCD)";
        return r;
    }

    // 3. Contains ':' → host:port.
    auto colon = trimmed.find(':');
    if (colon != std::string::npos) {
        std::string host_part = trimmed.substr(0, colon);
        std::string port_part = trimmed.substr(colon + 1);
        int port = 0;
        if (host_part.empty()) {
            r.type = InputType::Invalid;
            r.error = "Host part is empty (use 'host:port')";
            return r;
        }
        if (!looks_like_host(host_part)) {
            r.type = InputType::Invalid;
            r.error = "Invalid host: '" + host_part + "'";
            return r;
        }
        if (!parse_port(port_part, port)) {
            r.type = InputType::Invalid;
            r.error = "Invalid port: '" + port_part + "' (must be 1..65535)";
            return r;
        }
        r.type = InputType::IpPort;
        r.host = host_part;
        r.port = port;
        return r;
    }

    // 4. Pure digits → port number.
    int port = 0;
    if (parse_port(trimmed, port)) {
        r.type = InputType::Port;
        r.port = port;
        return r;
    }

    // 5. Anything else → invalid.
    r.type = InputType::Invalid;
    r.error = "Unrecognized input. Use a port (46318), host:port "
              "(192.168.1.10:46318), or room code (#ABCD)";
    return r;
}

const char* type_label(InputType t) {
    switch (t) {
        case InputType::Empty:    return "Empty (host with random port)";
        case InputType::Port:     return "Port (host on this port)";
        case InputType::IpPort:   return "IP:Port (direct join)";
        case InputType::RoomCode: return "Room code (relay join)";
        case InputType::Invalid:  return "Invalid";
    }
    return "?";
}

} // namespace caster::common::net::connection_detector
