// src/common/net/relay/relay_protocol.hpp
//
// Wire format for the NAT-traversal relay server. Ported from zzcaster's
// `src/net/relay_protocol.zig`.
//
// All integers are LITTLE-ENDIAN except STUN replies (big-endian per RFC 5389).

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

namespace caster::common::net::relay_protocol {

// Transport tag for HostRegister / ClientJoin messages.
inline constexpr std::uint8_t kTypeTcp = 'T';  // 0x54
inline constexpr std::uint8_t kTypeUdp = 'U';  // 0x55 (zzcaster always uses this)

// Room code constants.
inline constexpr int kRoomCodeLen = 4;
extern const char* kRoomCodeAlphabet;  // "ABCDEFGHJKLMNPQRSTUVWXYZ23456789"

// TCP message header magic strings.
inline constexpr const char* kMatchInfoHeader = "MatchInfo";  // 9 bytes
inline constexpr const char* kTunInfoHeader   = "TunInfo";    // 7 bytes
inline constexpr const char* kHostedHeader    = "Hosted";     // 6 bytes
inline constexpr const char* kErrorHeader     = "Error";      // 5 bytes
inline constexpr std::size_t kMatchInfoHeaderLen = 9;
inline constexpr std::size_t kTunInfoHeaderLen   = 7;
inline constexpr std::size_t kHostedHeaderLen    = 6;
inline constexpr std::size_t kErrorHeaderLen     = 5;

// Error codes (in the Error message body).
inline constexpr std::uint8_t kErrRoomNotFound    = 1;
inline constexpr std::uint8_t kErrRoomExpired     = 2;
inline constexpr std::uint8_t kErrProtocolError   = 3;
inline constexpr std::uint8_t kErrRoomTaken       = 4;

// Size limits.
inline constexpr std::size_t kMaxTunInfoAddrLen = 22;  // "255.255.255.255:65535"
inline constexpr std::size_t kMaxTunInfoLen     = kTunInfoHeaderLen + 4 + kMaxTunInfoAddrLen + 1;
inline constexpr std::size_t kMaxErrorMsgLen    = 64;
inline constexpr std::size_t kMaxErrorLen       = kErrorHeaderLen + 1 + kMaxErrorMsgLen;
inline constexpr std::size_t kMaxInitialMsgLen  = 64;
inline constexpr std::uint32_t kInvalidMatchId  = 0;

// STUN reply: 4 bytes IP (big-endian) + 2 bytes port (big-endian) + 2 bytes padding.
struct StunReply {
    std::uint8_t ip[4];
    std::uint16_t port;  // host byte order
};

// Tagged-union of server-to-client TCP messages.
enum class ServerMsgKind {
    Unknown,
    MatchInfo,
    TunInfo,
    Hosted,
    Error,
};

struct MatchInfoMsg {
    std::uint32_t match_id;
};

struct TunInfoMsg {
    std::uint32_t match_id;
    std::string   addr;  // "ip:port" string
};

struct HostedMsg {
    std::string code;  // exactly 4 chars
};

struct ErrorMsg {
    std::uint8_t code;
    std::string  msg;
};

struct ServerMsg {
    ServerMsgKind kind = ServerMsgKind::Unknown;
    MatchInfoMsg  match_info;
    TunInfoMsg    tun_info;
    HostedMsg     hosted;
    ErrorMsg      error;
};

// ---- Encoder functions --------------------------------------------------
// All write to `buf` and return the number of bytes written (0 on error).

std::size_t encode_host_register(char* buf, std::size_t buf_cap,
                                  std::uint8_t transport_type,
                                  std::uint16_t port,
                                  std::string_view code);

std::size_t encode_client_join(char* buf, std::size_t buf_cap,
                                std::uint8_t transport_type,
                                std::string_view code);

std::size_t encode_udp_data(char* buf, std::size_t buf_cap,
                             bool is_client, std::uint32_t match_id);

std::size_t encode_stun_probe(char* buf, std::size_t buf_cap);

// ---- Decoder functions --------------------------------------------------

// Decode a STUN reply (8 bytes). Returns nullopt on bad input.
std::optional<StunReply> decode_stun_reply(const std::uint8_t* data,
                                            std::size_t size);

// Decode a server TCP message. Returns a ServerMsg with kind=Unknown if
// the input doesn't match any known header or if TunInfo is fragmented
// (no null terminator yet).
ServerMsg decode_server_msg(const std::uint8_t* data, std::size_t size);

// Returns the number of bytes consumed by the last decoded message.
// Call this after decode_server_msg to know how much to shift the buffer.
std::size_t consumed_bytes(const ServerMsg& msg, std::size_t buf_size);

// ---- Room code utilities ------------------------------------------------

// Generate a random 4-char room code. `seed` should come from a high-res clock.
std::string generate_room_code(std::uint64_t seed);

// Validate a room code: exactly 4 chars, all from the alphabet.
bool is_valid_room_code(std::string_view code);

} // namespace caster::common::net::relay_protocol
