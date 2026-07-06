// src/dll/compression.cpp

#include "compression.hpp"

#define MINIZ_NO_ZLIB_COMPATIBLE_NAMES
#include <miniz.h>
#include <md5.h>

#include <cstring>

namespace caster::dll {

void getMD5(const char* bytes, size_t len, char dst[16]) {
    MD5_CTX md5;
    MD5_Init(&md5);
    MD5_Update(&md5, bytes, len);
    MD5_Final(reinterpret_cast<unsigned char*>(dst), &md5);
}

void getMD5(const std::string& str, char dst[16]) {
    getMD5(str.data(), str.size(), dst);
}

bool checkMD5(const char* bytes, size_t len, const char md5[16]) {
    char tmp[16];
    getMD5(bytes, len, tmp);
    return std::strncmp(tmp, md5, 16) == 0;
}

bool checkMD5(const std::string& str, const char md5[16]) {
    return checkMD5(str.data(), str.size(), md5);
}

size_t compress(const char* src, size_t srcLen, char* dst, size_t dstLen, int level) {
    mz_ulong len = dstLen;
    int rc = mz_compress2(reinterpret_cast<unsigned char*>(dst), &len,
                           reinterpret_cast<const unsigned char*>(src), srcLen, level);
    return (rc == MZ_OK) ? len : 0;
}

size_t uncompress(const char* src, size_t srcLen, char* dst, size_t dstLen) {
    mz_ulong len = dstLen;
    int rc = mz_uncompress(reinterpret_cast<unsigned char*>(dst), &len,
                            reinterpret_cast<const unsigned char*>(src), srcLen);
    return (rc == MZ_OK) ? len : 0;
}

size_t compressBound(size_t srcLen) {
    return mz_compressBound(srcLen);
}

} // namespace caster::dll
