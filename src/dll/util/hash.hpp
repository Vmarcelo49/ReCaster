// src/dll/util/hash.hpp
//
// 128-bit hashing via xxHash (XXH3_128bit) for sync-hash / desync detection.
//
// This replaces the previous compression.{hpp,cpp} module, which combined MD5
// hashing with zlib (miniz) compress/uncompress. Compression has been removed
// from the project entirely; only hashing remains, now backed by xxHash
// instead of MD5.

#pragma once

#include <cstddef>

namespace caster::dll {

// Compute a 128-bit hash of the given bytes and write the 16 raw bytes
// into dst. Output is byte-identical for byte-identical input across
// platforms (xxHash is little-endian deterministic for XXH3_128bits).
void getHash(const char* bytes, std::size_t len, char dst[16]);

} // namespace caster::dll
