// src/common/net/relay/relay_protocol.cpp

#include "relay_protocol.hpp"
#include "../../logger.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <string>

namespace caster::common::net::relay_protocol {

const char* kRoomCodeAlphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";

namespace {

void write_u16_le(char* p, std::uint16_t v) {
    p[0] = static_cast<char>(v & 0xff);
    p[1] = static_cast<char>((v >> 8) & 0xff);
}

void write_u32_le(char* p, std::uint32_t v) {
    p[0] = static_cast<char>(v & 0xff);
    p[1] = static_cast<char>((v >> 8) & 0xff);
    p[2] = static_cast<char>((v >> 16) & 0xff);
    p[3] = static_cast<char>((v >> 24) & 0xff);
}

std::uint16_t read_u16_le(const std::uint8_t* p) {
    return static_cast<std::uint16_t>(p[0]) |
           (static_cast<std::uint16_t>(p[1]) << 8);
}

std::uint32_t read_u32_le(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) |
           (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) |
           (static_cast<std::uint32_t>(p[3]) << 24);
}

// Big-endian read for STUN (RFC 5389).
std::uint16_t read_u16_be(const std::uint8_t* p) {
    return (static_cast<std::uint16_t>(p[0]) << 8) |
           static_cast<std::uint16_t>(p[1]);
}

bool starts_with(const std::uint8_t* data, std::size_t size,
                 const char* header, std::size_t header_len) {
    if (size < header_len) return false;
    return std::memcmp(data, header, header_len) == 0;
}

} // namespace

// ---- Encoders -----------------------------------------------------------

std::size_t encode_host_register(char* buf, std::size_t buf_cap,
                                  std::uint8_t transport_type,
                                  std::uint16_t port,
                                  std::string_view code) {
    if (transport_type != kTypeTcp && transport_type != kTypeUdp) return 0;
    if (code.size() > 4) return 0;
    // Layout: [u8 type][u16 le port][u8 code_len][code bytes]
    const std::size_t total = 1 + 2 + 1 + code.size();
    if (buf_cap < total) return 0;

    std::size_t i = 0;
    buf[i++] = static_cast<char>(transport_type);
    write_u16_le(buf + i, port);
    i += 2;
    buf[i++] = static_cast<char>(code.size());
    if (!code.empty()) {
        std::memcpy(buf + i, code.data(), code.size());
        i += code.size();
    }
    return i;
}

std::size_t encode_client_join(char* buf, std::size_t buf_cap,
                                std::uint8_t transport_type,
                                std::string_view code) {
    if (transport_type != kTypeTcp && transport_type != kTypeUdp) return 0;
    if (code.size() != 4) return 0;
    // Layout: [u8 type][u8 code_len=4][code 4 bytes]
    const std::size_t total = 1 + 1 + 4;
    if (buf_cap < total) return 0;

    std::size_t i = 0;
    buf[i++] = static_cast<char>(transport_type);
    buf[i++] = static_cast<char>(code.size());
    std::memcpy(buf + i, code.data(), 4);
    i += 4;
    return i;
}

std::size_t encode_udp_data(char* buf, std::size_t buf_cap,
                             bool is_client, std::uint32_t match_id) {
    if (match_id == kInvalidMatchId) return 0;
    // Layout: [u8 isClient 0|1][u32 le matchId]  (5 bytes)
    if (buf_cap < 5) return 0;
    buf[0] = is_client ? 1 : 0;
    write_u32_le(buf + 1, match_id);
    return 5;
}

std::size_t encode_stun_probe(char* buf, std::size_t buf_cap) {
    if (buf_cap < 1) return 0;
    buf[0] = 'X';  // 0x58
    return 1;
}

// ---- Decoders -----------------------------------------------------------

std::optional<StunReply> decode_stun_reply(const std::uint8_t* data,
                                            std::size_t size) {
    if (size < 8) return std::nullopt;
    StunReply r;
    std::memcpy(r.ip, data, 4);
    // Port is BIG-ENDIAN per RFC 5389.
    r.port = read_u16_be(data + 4);
    // Bytes [6..8] = padding, ignored.
    return r;
}

ServerMsg decode_server_msg(const std::uint8_t* data, std::size_t size) {
    ServerMsg msg;

    // MatchInfo: "MatchInfo" (9) + u32 le matchId  = 13 bytes
    if (starts_with(data, size, kMatchInfoHeader, kMatchInfoHeaderLen)) {
        if (size < kMatchInfoHeaderLen + 4) return msg;  // fragmented
        msg.kind = ServerMsgKind::MatchInfo;
        msg.match_info.match_id = read_u32_le(data + kMatchInfoHeaderLen);
        return msg;
    }

    // TunInfo: "TunInfo" (7) + u32 le matchId + addr bytes + NUL
    if (starts_with(data, size, kTunInfoHeader, kTunInfoHeaderLen)) {
        const std::size_t header_and_id = kTunInfoHeaderLen + 4;
        if (size < header_and_id) return msg;  // fragmented
        // Find the NUL terminator after the addr.
        const std::uint8_t* addr_start = data + header_and_id;
        const std::size_t addr_max = size - header_and_id;
        std::size_t addr_len = 0;
        while (addr_len < addr_max && addr_start[addr_len] != 0) {
            ++addr_len;
        }
        if (addr_len >= addr_max) {
            // No NUL found yet — fragmented.
            return msg;
        }
        msg.kind = ServerMsgKind::TunInfo;
        msg.tun_info.match_id = read_u32_le(data + kTunInfoHeaderLen);
        msg.tun_info.addr.assign(
            reinterpret_cast<const char*>(addr_start), addr_len);
        return msg;
    }

    // Hosted: "Hosted" (6) + 4 bytes code
    if (starts_with(data, size, kHostedHeader, kHostedHeaderLen)) {
        if (size < kHostedHeaderLen + 4) return msg;  // fragmented
        msg.kind = ServerMsgKind::Hosted;
        msg.hosted.code.assign(
            reinterpret_cast<const char*>(data + kHostedHeaderLen), 4);
        return msg;
    }

    // Error: "Error" (5) + u8 code + msg bytes (no length prefix, no NUL)
    if (starts_with(data, size, kErrorHeader, kErrorHeaderLen)) {
        if (size < kErrorHeaderLen + 1) return msg;  // fragmented
        msg.kind = ServerMsgKind::Error;
        msg.error.code = data[kErrorHeaderLen];
        // Message is everything after the code byte, capped at kMaxErrorMsgLen.
        const std::size_t msg_start = kErrorHeaderLen + 1;
        std::size_t msg_len = std::min(size - msg_start, kMaxErrorMsgLen);
        msg.error.msg.assign(
            reinterpret_cast<const char*>(data + msg_start), msg_len);
        return msg;
    }

    // Unknown.
    return msg;
}

std::size_t consumed_bytes(const ServerMsg& msg, std::size_t buf_size) {
    switch (msg.kind) {
        case ServerMsgKind::MatchInfo:
            return kMatchInfoHeaderLen + 4;  // 13
        case ServerMsgKind::TunInfo:
            // 7 (header) + 4 (matchId) + addr.len + 1 (NUL)
            return kTunInfoHeaderLen + 4 + msg.tun_info.addr.size() + 1;
        case ServerMsgKind::Hosted:
            return kHostedHeaderLen + 4;  // 10
        case ServerMsgKind::Error:
            // 5 (header) + 1 (code) + min(msg.size, kMaxErrorMsgLen)
            return kErrorHeaderLen + 1 +
                   std::min(msg.error.msg.size(), kMaxErrorMsgLen);
        case ServerMsgKind::Unknown:
        default:
            // Don't consume anything if we couldn't parse.
            return 0;
    }
}

// ---- Room code utilities ------------------------------------------------

std::string generate_room_code(std::uint64_t seed) {
    // Simple LCG from the seed — good enough for room codes (4 chars
    // from a 32-char alphabet = 2^20 = ~1M possibilities per seed).
    std::uint64_t state = seed;
    auto next = [&]() {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        return static_cast<std::uint32_t>(state >> 32);
    };

    constexpr int kAlphabetLen = 32;
    std::string code(kRoomCodeLen, '\0');
    for (int i = 0; i < kRoomCodeLen; ++i) {
        code[i] = kRoomCodeAlphabet[next() % kAlphabetLen];
    }
    return code;
}

bool is_valid_room_code(std::string_view code) {
    if (code.size() != kRoomCodeLen) return false;
    for (char c : code) {
        // Uppercase letters (A-Z) or digits (0-9) only.
        if (!(std::isupper(static_cast<unsigned char>(c)) ||
              std::isdigit(static_cast<unsigned char>(c)))) return false;
    }
    return true;
}

} // namespace caster::common::net::relay_protocol
