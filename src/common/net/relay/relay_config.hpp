// src/common/net/relay/relay_config.hpp
//
// Parser for the relay server list. Reads from a multi-line string
// (typically from config.ini [network] relay_servers field).
//
// Ported from zzcaster's `src/net/relay_config.zig`.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace caster::common::net::relay_config {

struct RelayEntry {
    std::string   host;
    std::uint16_t port;
    std::string format_addr() const { return host + ":" + std::to_string(port); }
};

using RelayList = std::vector<RelayEntry>;

// Default port for relay servers (matches zzcaster).
inline constexpr std::uint16_t kDefaultRelayPort = 3939;

// Built-in default relay list (used when user config has no relays).
// Format: one entry per line, # comments allowed.
extern const char* kDefaultRelayList;

// Parse a single line. Returns nullopt if the line is empty or a comment.
// Format: "host" or "host:port" (whitespace trimmed). Port defaults to 3939.
std::vector<RelayEntry>::value_type parse_line(std::string line);

// Parse a multi-line string into a RelayList. Skips comments and blanks.
RelayList parse_list(const std::string& content);

// Get the default list (parsed from kDefaultRelayList).
RelayList default_list();

} // namespace caster::common::net::relay_config
