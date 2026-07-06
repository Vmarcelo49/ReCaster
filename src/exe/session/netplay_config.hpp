// src/exe/session/netplay_config.hpp
//
// Snapshot of the netplay configuration that gets sent to the DLL via IPC
// after the handshake completes. Ported from zzcaster's
// `session.zig::NetplayConfig`.

#pragma once

#include <cstdint>
#include <string>

namespace caster::exe::session {

struct NetplayConfig {
    bool          is_host            = false;
    bool          is_training        = false;
    bool          is_spectator       = false;
    std::uint8_t  delay              = 0;
    bool          manual_delay       = false;
    std::uint8_t  rollback           = 0;
    std::uint8_t  rollback_delay     = 0;
    std::uint8_t  win_count          = 2;
    std::uint32_t match_seed         = 0;
    std::uint8_t  host_player        = 1;
    std::uint8_t  local_player       = 1;
    std::uint8_t  remote_player      = 2;
    std::string   peer_addr;          // "ip" (no port) for DLL
    std::uint16_t peer_port          = 0;
    bool          is_netplay         = false;
    std::uint16_t spectator_listen_port = 0;
    std::string   local_name;
    std::string   remote_name;
    std::string   local_connection_type;   // "Wired" / "Wireless" / "Unknown"
    std::string   remote_connection_type;
    std::uint16_t local_udp_port      = 0;
};

struct PingStats {
    double       avg_ms         = 0;
    double       min_ms         = 0;
    double       max_ms         = 0;
    std::uint32_t count         = 0;
    std::uint8_t packet_loss    = 0;
};

} // namespace caster::exe::session
