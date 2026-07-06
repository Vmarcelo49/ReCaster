// src/common/net/enet_transport.cpp

#include "enet_transport.hpp"
#include "../logger.hpp"

#include <enet/enet.h>

#include <cstring>

namespace caster::common::net {

EnetTransport::~EnetTransport() {
    deinit();
}

bool EnetTransport::ensure_enet_initialized(std::string& error) {
    if (owns_enet_init_) return true;
    if (enet_initialize() != 0) {
        error = "enet_initialize() failed";
        return false;
    }
    owns_enet_init_ = true;
    return true;
}

bool EnetTransport::listen(std::uint16_t port, std::string& error) {
    if (host_) {
        error = "transport already has a host";
        return false;
    }
    if (!ensure_enet_initialized(error)) return false;

    ENetAddress addr;
    addr.host = ENET_HOST_ANY;
    addr.port = port;
    // 2 peers, 2 channels, unlimited bandwidth.
    host_ = enet_host_create(&addr, 2, 2, 0, 0);
    if (!host_) {
        error = "enet_host_create(listen) failed (port " +
                std::to_string(port) + " in use?)";
        return false;
    }
    is_host_ = true;
    logger::info("enet: listening on port {}", port);
    return true;
}

bool EnetTransport::connect(const std::string& host_str, std::uint16_t port,
                             std::string& error) {
    return connect_bound(host_str, port, 0, error);
}

bool EnetTransport::connect_bound(const std::string& host_str,
                                   std::uint16_t port,
                                   std::uint16_t local_port,
                                   std::string& error) {
    if (host_) {
        error = "transport already has a host";
        return false;
    }
    if (!ensure_enet_initialized(error)) return false;

    // Bind to local_port (0 = OS-chosen).
    ENetAddress bind_addr;
    bind_addr.host = ENET_HOST_ANY;
    bind_addr.port = local_port;
    // 1 peer, 2 channels, unlimited bandwidth.
    host_ = enet_host_create(&bind_addr, 1, 2, 0, 0);
    if (!host_) {
        error = "enet_host_create(connect, local_port=" +
                std::to_string(local_port) + ") failed";
        return false;
    }

    ENetAddress peer_addr;
    if (enet_address_set_host(&peer_addr, host_str.c_str()) != 0) {
        error = "enet_address_set_host('" + host_str + "') failed";
        enet_host_destroy(host_);
        host_ = nullptr;
        return false;
    }
    peer_addr.port = port;
    peer_ = enet_host_connect(host_, &peer_addr, 2, 0);
    if (!peer_) {
        error = "enet_host_connect failed";
        enet_host_destroy(host_);
        host_ = nullptr;
        return false;
    }
    // Set peer timeouts: 0 = no timeout on connect, 30000ms idle, 120000ms global.
    enet_peer_timeout(peer_, 0, 30000, 120000);
    is_host_ = false;
    logger::info("enet: connecting to {}:{} (local_port={})",
                 host_str, port, local_port);
    return true;
}

bool EnetTransport::send_reliable(const void* data, std::size_t size) {
    if (!peer_ || !connected_) return false;
    ENetPacket* pkt = enet_packet_create(data, size, ENET_PACKET_FLAG_RELIABLE);
    if (!pkt) return false;
    if (enet_peer_send(peer_, 0, pkt) < 0) {
        enet_packet_destroy(pkt);
        return false;
    }
    enet_host_flush(host_);
    return true;
}

bool EnetTransport::send_unreliable(const void* data, std::size_t size) {
    if (!peer_ || !connected_) return false;
    ENetPacket* pkt = enet_packet_create(data, size, 0);
    if (!pkt) return false;
    if (enet_peer_send(peer_, 1, pkt) < 0) {
        enet_packet_destroy(pkt);
        return false;
    }
    enet_host_flush(host_);
    return true;
}

bool EnetTransport::poll(std::uint32_t timeout_ms, TransportEvent& out) {
    if (!host_) return false;

    ENetEvent ev;
    int rc = enet_host_service(host_, &ev, timeout_ms);
    if (rc <= 0) return false;  // 0 = timeout, <0 = error

    switch (ev.type) {
        case ENET_EVENT_TYPE_CONNECT:
            peer_ = ev.peer;
            connected_ = true;
            // Use the same timeouts as connect(): 30s/120s.
            enet_peer_timeout(peer_, 0, 30000, 120000);
            out = TransportEvent::Connected;
            logger::info("enet: peer connected");
            return true;
        case ENET_EVENT_TYPE_RECEIVE: {
            const std::size_t n = std::min<std::size_t>(
                ev.packet->dataLength, kLastMessageCap);
            std::memcpy(last_message_, ev.packet->data, n);
            last_message_len_ = n;
            enet_packet_destroy(ev.packet);
            out = TransportEvent::MessageReceived;
            return true;
        }
        case ENET_EVENT_TYPE_DISCONNECT:
            peer_ = nullptr;
            connected_ = false;
            out = TransportEvent::Disconnected;
            logger::info("enet: peer disconnected");
            return true;
        default:
            return false;
    }
}

void EnetTransport::ping() {
    if (peer_) enet_peer_ping(peer_);
}

TransportStats EnetTransport::get_stats() const {
    TransportStats s;
    if (!peer_) return s;
    s.rtt_ms          = peer_->roundTripTime;
    s.jitter_ms       = peer_->lastRoundTripTimeVariance;
    s.packet_loss_pct = static_cast<std::uint32_t>(
        peer_->packetLoss / ENET_PEER_PACKET_LOSS_SCALE);
    return s;
}

void EnetTransport::deinit() {
    if (peer_) {
        enet_peer_disconnect(peer_, 0);
        enet_host_flush(host_);
        peer_ = nullptr;
    }
    connected_ = false;
    if (host_) {
        enet_host_destroy(host_);
        host_ = nullptr;
    }
    is_host_ = false;
    if (owns_enet_init_) {
        enet_deinitialize();
        owns_enet_init_ = false;
    }
}

} // namespace caster::common::net
