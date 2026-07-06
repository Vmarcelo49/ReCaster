// src/common/net/connection_detector.hpp
//
// Parser for the unified netplay input field. Classifies a user-typed
// string into one of 5 categories, with extracted fields.
//
// Ported from zzcaster's `net/connection_detector.zig`.
//
// The input field accepts:
//   - Empty              → "host with a random port" (smart host)
//   - "46318"            → "host on port 46318" (smart host with specific port)
//   - "192.168.1.10:46318" → "direct join at host:port"
//   - "#ABCD"            → "relay join with room code ABCD"
//   - Anything else      → Invalid
//
// Room codes are exactly 4 uppercase ASCII letters after the '#'.

#pragma once

#include <string>

namespace caster::common::net::connection_detector {

enum class InputType {
    Empty,
    Port,         // just a port number (1..65535)
    IpPort,       // host:port
    RoomCode,     // #ABCD (4 uppercase letters)
    Invalid,
};

struct ParseResult {
    InputType   type      = InputType::Invalid;
    std::string host;        // only for IpPort
    int         port     = 0; // for Port (the port) and IpPort
    std::string room_code;   // only for RoomCode (4 letters, no '#')
    std::string error;       // human-readable, only when type == Invalid
};

// Parse `input` into a ParseResult. Never throws.
ParseResult parse_input(const std::string& input);

// Convenience: classify without extracting fields.
inline InputType classify(const std::string& input) {
    return parse_input(input).type;
}

// Human-readable label for each type (for UI display).
const char* type_label(InputType t);

} // namespace caster::common::net::connection_detector
