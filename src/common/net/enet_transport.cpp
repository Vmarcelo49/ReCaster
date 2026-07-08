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

bool EnetTransport::bind_only(std::uint16_t local_port, std::string& error) {
    if (host_) {
        error = "transport already has a host";
        return false;
    }
    if (!ensure_enet_initialized(error)) return false;

    ENetAddress bind_addr;
    bind_addr.host = ENET_HOST_ANY;
    bind_addr.port = local_port;
    // 1 peer (the relay-routed opponent), 2 channels, unlimited bandwidth.
    // We do NOT call enet_host_connect here — the peer is attached later
    // by connect_to_peer() once the relay hole-punch has opened the NAT.
    host_ = enet_host_create(&bind_addr, 1, 2, 0, 0);
    if (!host_) {
        error = "enet_host_create(bind_only, local_port=" +
                std::to_string(local_port) + ") failed";
        return false;
    }
    is_host_ = false;
    logger::info("enet: bound to local port {} (no peer yet — relay path)",
                 local_port);
    return true;
}

bool EnetTransport::connect_to_peer(const std::string& host_str,
                                     std::uint16_t port,
                                     std::string& error) {
    if (!host_) {
        error = "connect_to_peer() called without bind_only()";
        return false;
    }
    if (peer_) {
        error = "transport already has a peer";
        return false;
    }

    ENetAddress peer_addr;
    if (enet_address_set_host(&peer_addr, host_str.c_str()) != 0) {
        error = "enet_address_set_host('" + host_str + "') failed";
        return false;
    }
    peer_addr.port = port;
    peer_ = enet_host_connect(host_, &peer_addr, 2, 0);
    if (!peer_) {
        error = "enet_host_connect failed";
        return false;
    }
    enet_peer_timeout(peer_, 0, 30000, 120000);
    logger::info("enet: connecting to {}:{} (post-hole-punch)",
                 host_str, port);
    return true;
}

int EnetTransport::udp_socket_fd() const {
    if (!host_) return -1;
    // ENet's ENetHost exposes its bound UDP socket as `socket`. On Windows
    // ENetSocket is SOCKET (unsigned); we follow the project's int-with-
    // -1-sentinel convention used throughout RelayClient. Sockets handed
    // out by the OS are always small positive ints in practice.
    return static_cast<int>(host_->socket);
}

// ---- Relay intercept -----------------------------------------------------
//
// ENet's intercept callback runs inside enet_protocol_receive_incoming_
// commands, right after enet_socket_receive() reads a packet and before
// enet_protocol_handle_incoming_commands() processes it. At that point
// host->receivedAddress / receivedData / receivedDataLength hold the raw
// packet. Returning 1 makes ENet treat the packet as handled (skip);
// returning 0 lets ENet process it.
//
// We use it to make ENet the SOLE reader of the shared UDP socket: when
// the relay client reuses this socket, its probe packets (the peer's
// 1-byte NullMsg) are routed to the relay client here, and everything
// else (including the peer's real ENet CONNECT) flows to ENet.

namespace {
// At most one EnetTransport has an intercept installed per process in
// this codebase (the netplay session owns a single transport). The thunk
// recovers its EnetTransport* through this pointer.
EnetTransport* g_intercept_owner = nullptr;
} // namespace

int EnetTransport::dispatch_intercept(EnetTransport* self, _ENetHost* host) {
    if (!self || !self->relay_sink_) return 0;
    ENetHost* h = reinterpret_cast<ENetHost*>(host);
    if (h->receivedDataLength == 0 || h->receivedData == nullptr) return 0;

    // ENetAddress.host is the sender's IPv4 in network byte order (a
    // plain enet_uint32 storing the in_addr). Port is host byte order —
    // see enet_socket_receive in win32.c / unix.c which fills
    // address->port = ntohs(sin.sin_port).
    bool consumed = self->relay_sink_->inject_received_packet(
        h->receivedData, h->receivedDataLength,
        h->receivedAddress.host, h->receivedAddress.port);
    return consumed ? 1 : 0;
}

namespace {
int ENET_CALLBACK enet_intercept_thunk(ENetHost* host, ENetEvent* /*event*/) {
    return EnetTransport::dispatch_intercept(g_intercept_owner,
                                              reinterpret_cast<_ENetHost*>(host));
}
} // namespace

void EnetTransport::install_intercept() {
    if (!host_) return;
    if (!relay_sink_) return;
    g_intercept_owner = this;
    host_->intercept = enet_intercept_thunk;
    logger::info("enet: relay intercept installed (sole-socket-reader mode)");
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
