// src/common/ipc/config_buffer.hpp
//
// Wire format of the launcher → DLL config message. This is the "IPC v4"
// contract: launcher writes the message, DLL reads it on its worker thread
// after hooking up.
//
// Layout (all integers little-endian, no padding):
//
//   Offset  Size  Field            Notes
//   ------  ----  ---------------  ----------------------------------------
//   0       1     flags            bit0=training, bit1=netplay,
//                                   bit2=host, bit3=spectator
//   1       1     delay            input delay (frames, 0..8)
//   2       1     rollback         rollback window (frames)
//   3       1     win_count        best-of (e.g. 2 = first to 2)
//   4       1     host_player      1 or 2 (which side is hosting)
//   5       2     peer_port        UDP port of the peer (LE u16)
//   7       2     local_udp_port   UDP port we bound locally (LE u16)
//   9       4     match_seed       deterministic RNG seed (LE u32)
//   13      N     peer_addr        NUL-terminated UTF-8 string, max 243
//                                   bytes (so total <= 256 bytes)
//   13+N+1  1     local_name_len   length of local_name (0..31)
//   14+N+1  M     local_name       UTF-8, not NUL-terminated (M = local_name_len)
//   14+N+1+M 1    remote_name_len  length of remote_name (0..31)
//   15+N+1+M R    remote_name      UTF-8, not NUL-terminated (R = remote_name_len)
//
// Total: 15 + N + M + R bytes, where 0 <= N <= 243, 0 <= M,R <= 31.
// Buffer max size = 320 (allows max names + max peer_addr).
//
// v1 = first 9 bytes (offline only). v2 added peer_addr.
// v3 = adds match_seed (4 bytes at offset 9) and moves peer_addr to offset 13.
// v4 = adds local_name + remote_name (length-prefixed strings after peer_addr).
//
// We only emit v4. The DLL can detect v3 by length (no name fields after
// peer_addr) and treat missing names as empty.

#pragma once

#include <cstdint>
#include <string>

namespace caster::common::ipc::config_buffer {

// Bit masks for the `flags` byte.
inline constexpr std::uint8_t kFlagTraining   = 0x01;
inline constexpr std::uint8_t kFlagNetplay    = 0x02;
inline constexpr std::uint8_t kFlagHost       = 0x04;
inline constexpr std::uint8_t kFlagSpectator  = 0x08;
inline constexpr std::uint8_t kFlagPlayernameEnabled = 0x10;  // show playername overlay
inline constexpr std::uint8_t kFlagPlayernameBottom  = 0x20;  // position: 0=top, 1=bottom

inline constexpr std::size_t kMaxPeerAddrLen = 243;
inline constexpr std::size_t kMaxNameLen    = 31;
inline constexpr std::size_t kMaxBufferSize = 321;  // 13 + 243 + 1 + 1 + 31 + 1 + 31
inline constexpr std::size_t kHeaderSize     = 13;  // bytes before peer_addr

// Strongly-typed config struct used by the launcher to build the buffer
// and by the DLL to parse it.
struct Config {
    std::uint8_t  flags          = 0;
    std::uint8_t  delay          = 0;
    std::uint8_t  rollback       = 4;
    std::uint8_t  win_count      = 2;
    std::uint8_t  host_player    = 1;
    std::uint16_t peer_port      = 0;
    std::uint16_t local_udp_port = 0;
    std::uint32_t match_seed     = 0;
    std::string   peer_addr;     // UTF-8, no null terminator (added on wire)
    std::string   local_name;    // UTF-8 player name (max 31 bytes)
    std::string   remote_name;   // UTF-8 opponent name (max 31 bytes)

    // Flag helpers (defined inline so they're available wherever Config is used).
    bool is_training()  const { return (flags & kFlagTraining)  != 0; }
    bool is_netplay()   const { return (flags & kFlagNetplay)   != 0; }
    bool is_host()      const { return (flags & kFlagHost)      != 0; }
    bool is_spectator() const { return (flags & kFlagSpectator) != 0; }
    bool playername_enabled()        const { return (flags & kFlagPlayernameEnabled) != 0; }
    bool playername_position_bottom() const { return (flags & kFlagPlayernameBottom)  != 0; }
};

// Serialize `cfg` into `out` (which must have capacity >= kMaxBufferSize).
// Returns the number of bytes written: 16 + peer_addr.size() + local_name.size()
// + remote_name.size() (including NUL terminator for peer_addr and 1-byte
// length prefixes for each name). Truncates fields that exceed their max.
std::size_t serialize(const Config& cfg, std::uint8_t* out, std::size_t out_cap);

// Parse a buffer received over the pipe. Returns true on success.
// Rejects buffers smaller than kHeaderSize or larger than kMaxBufferSize.
// peer_addr is NUL-terminated; local_name and remote_name are
// length-prefixed (1-byte length + data, no NUL).
bool deserialize(const std::uint8_t* buf, std::size_t size, Config& out);

// Convenience: serialize into a std::string (caller-friendly version).
std::string serialize_to_string(const Config& cfg);

} // namespace caster::common::ipc::config_buffer
