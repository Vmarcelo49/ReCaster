// src/common/net/ip_discovery.hpp
//
// Discover the local machine's public IP (via HTTPS to api.ipify.org)
// and local IP (via gethostname + gethostbyname). Windows-only.

#pragma once

#include <string>

namespace caster::common::net::ip_discovery {

// Get the public IP via wininet HTTP GET to https://api.ipify.org.
// Returns empty string on failure. `buf` is filled with up to 64 chars.
std::string get_public_ip();

// Get the local IP (first IPv4 address of the default interface).
// Returns empty string on failure.
std::string get_local_ip();

} // namespace caster::common::net::ip_discovery
