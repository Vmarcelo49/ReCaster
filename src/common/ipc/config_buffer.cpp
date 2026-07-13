// src/common/ipc/config_buffer.cpp

#include "config_buffer.hpp"
#include "../logger.hpp"

#include <cstring>

namespace caster::common::ipc::config_buffer {

namespace {

void write_u16_le(std::uint8_t* p, std::uint16_t v) {
    p[0] = static_cast<std::uint8_t>(v & 0xff);
    p[1] = static_cast<std::uint8_t>((v >> 8) & 0xff);
}

void write_u32_le(std::uint8_t* p, std::uint32_t v) {
    p[0] = static_cast<std::uint8_t>(v & 0xff);
    p[1] = static_cast<std::uint8_t>((v >> 8) & 0xff);
    p[2] = static_cast<std::uint8_t>((v >> 16) & 0xff);
    p[3] = static_cast<std::uint8_t>((v >> 24) & 0xff);
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

} // namespace

std::size_t serialize(const Config& cfg, std::uint8_t* out, std::size_t out_cap) {
    if (out_cap < kHeaderSize + 1) {
        logger::err("config_buffer: out_cap too small ({})", out_cap);
        return 0;
    }

    // Truncate peer_addr if needed.
    std::size_t addr_len = cfg.peer_addr.size();
    if (addr_len > kMaxPeerAddrLen) {
        logger::warn("config_buffer: peer_addr truncated from {} to {}",
                     addr_len, kMaxPeerAddrLen);
        addr_len = kMaxPeerAddrLen;
    }

    // Truncate names if needed.
    std::size_t local_len = cfg.local_name.size();
    if (local_len > kMaxNameLen) local_len = kMaxNameLen;
    std::size_t remote_len = cfg.remote_name.size();
    if (remote_len > kMaxNameLen) remote_len = kMaxNameLen;

    // Total: header + peer_addr + NUL + local_name_len + local_name + remote_name_len + remote_name
    const std::size_t total = kHeaderSize + addr_len + 1 + 1 + local_len + 1 + remote_len;
    if (total > out_cap) {
        logger::err("config_buffer: total {} > out_cap {}", total, out_cap);
        return 0;
    }

    std::size_t i = 0;
    out[i++] = cfg.flags;
    out[i++] = cfg.delay;
    out[i++] = cfg.rollback;
    out[i++] = cfg.win_count;
    out[i++] = cfg.host_player;
    write_u16_le(out + i, cfg.peer_port);       i += 2;
    write_u16_le(out + i, cfg.local_udp_port);  i += 2;
    write_u32_le(out + i, cfg.match_seed);      i += 4;

    // peer_addr: copy bytes, then NUL terminator.
    if (addr_len) {
        std::memcpy(out + i, cfg.peer_addr.data(), addr_len);
        i += addr_len;
    }
    out[i++] = 0;  // NUL terminator

    // local_name: length-prefixed (1 byte length + data, no NUL).
    out[i++] = static_cast<std::uint8_t>(local_len);
    if (local_len) {
        std::memcpy(out + i, cfg.local_name.data(), local_len);
        i += local_len;
    }

    // remote_name: length-prefixed (1 byte length + data, no NUL).
    out[i++] = static_cast<std::uint8_t>(remote_len);
    if (remote_len) {
        std::memcpy(out + i, cfg.remote_name.data(), remote_len);
        i += remote_len;
    }

    return i;
}

bool deserialize(const std::uint8_t* buf, std::size_t size, Config& out) {
    if (size < kHeaderSize) {
        logger::err("config_buffer: buffer too small ({})", size);
        return false;
    }
    if (size > kMaxBufferSize) {
        logger::err("config_buffer: buffer too large ({})", size);
        return false;
    }

    out.flags          = buf[0];
    out.delay          = buf[1];
    out.rollback       = buf[2];
    out.win_count      = buf[3];
    out.host_player    = buf[4];
    out.peer_port      = read_u16_le(buf + 5);
    out.local_udp_port = read_u16_le(buf + 7);
    out.match_seed     = read_u32_le(buf + 9);

    // peer_addr: from offset 13 to the first NUL (or end of buffer).
    std::size_t i = kHeaderSize;
    if (size > kHeaderSize) {
        const std::uint8_t* addr_start = buf + kHeaderSize;
        const std::size_t addr_max = size - kHeaderSize;
        std::size_t addr_len = 0;
        while (addr_len < addr_max && addr_start[addr_len] != 0) {
            ++addr_len;
        }
        out.peer_addr.assign(reinterpret_cast<const char*>(addr_start), addr_len);
        i = kHeaderSize + addr_len + 1;  // skip addr + NUL
    } else {
        out.peer_addr.clear();
    }

    // local_name + remote_name (v4 format). If the buffer ends right after
    // peer_addr's NUL (v3 format), names stay empty — backward compatible.
    out.local_name.clear();
    out.remote_name.clear();

    if (i + 1 <= size) {
        const std::uint8_t local_len = buf[i++];
        if (i + local_len <= size) {
            out.local_name.assign(reinterpret_cast<const char*>(buf + i), local_len);
            i += local_len;
        }
    }

    if (i + 1 <= size) {
        const std::uint8_t remote_len = buf[i++];
        if (i + remote_len <= size) {
            out.remote_name.assign(reinterpret_cast<const char*>(buf + i), remote_len);
            i += remote_len;
        }
    }

    return true;
}

std::string serialize_to_string(const Config& cfg) {
    std::string out(kMaxBufferSize, '\0');
    std::size_t n = serialize(cfg, reinterpret_cast<std::uint8_t*>(out.data()),
                              out.size());
    out.resize(n);
    return out;
}

} // namespace caster::common::ipc::config_buffer
