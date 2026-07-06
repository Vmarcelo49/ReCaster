// src/common/net/connection_type.hpp
//
// Detects whether the default network route is Wi-Fi or Ethernet.
// Returns "Wired" / "Wireless" / "Unknown". Used by the netplay session
// to warn the user about Wi-Fi latency.
//
// Ported from zzcaster's `src/launcher/net_util.zig`. Includes Wine
// fallback that reads /proc/net/route and /sys/class/net/<iface>/wireless
// via Z:\ path translation.

#pragma once

#include <string>

namespace caster::common::net::connection_type {

// Returns "Wired", "Wireless", or "Unknown".
std::string get_connection_type();

} // namespace caster::common::net::connection_type
