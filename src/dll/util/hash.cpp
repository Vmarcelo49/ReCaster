// src/dll/util/hash.cpp
//
// 128-bit hashing via xxHash (XXH3_128bit). Used by SyncHash::readFromGame
// for desync detection during rollback netplay.
//
// XXH_INLINE_ALL is defined so that all xxHash symbols become static inline
// within this translation unit. No separate xxhash.c compilation is needed;
// only the xxhash.h header is required on the include path.

#include "hash.hpp"

#define XXH_INLINE_ALL
#include <xxhash.h>

#include <cstring>

namespace caster::dll {

void getHash(const char* bytes, std::size_t len, char dst[16]) {
    // XXH3_128bits returns XXH128_hash_t { uint64_t low64; uint64_t high64; }
    // which is exactly 16 bytes. We memcpy to avoid strict-aliasing issues.
    XXH128_hash_t h = XXH3_128bits(bytes, len);
    std::memcpy(dst, &h, 16);
}

} // namespace caster::dll
