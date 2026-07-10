// src/common/net/enet_transport.hpp
//
// Thin C++ wrapper around the ENet library. Ported from zzcaster's
// `src/net/enet_transport.zig`. Owns an ENetHost + (optional) ENetPeer.
//
// Lifecycle:
//   EnetTransport t;
//   t.listen(46318);          // host mode
//   // or:
//   t.connect("1.2.3.4", 46318);          // client mode
//   // or:
//   t.connect_bound("1.2.3.4", 46318, 51234);  // client with fixed local port (relay handoff)
//
//   while (running) {
//       TransportEvent ev;
//       if (t.poll(0, ev)) {
//           switch (ev) {
//               case Connected:    ...
//               case Disconnected: ...
//               case MessageReceived: t.last_message()
//           }
//       }
//       t.send_reliable(data, size);
//   }
//   t.deinit();
//
// Threading: not thread-safe. Caller must serialize calls.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

// Forward decls — we don't want to leak enet.h into this header.
struct _ENetHost;
struct _ENetPeer;

namespace caster::common::net {

enum class TransportEvent {
    Connected,
    Disconnected,
    TimedOut,           // defined but never emitted (poll returns false on timeout)
    MessageReceived,
    ErrState,           // defined but never emitted
};

struct TransportStats {
    std::uint32_t rtt_ms        = 0;
    std::uint32_t jitter_ms     = 0;
    std::uint32_t packet_loss_pct = 0;
};

class EnetTransport {
public:
    EnetTransport() = default;
    ~EnetTransport();

    EnetTransport(const EnetTransport&)            = delete;
    EnetTransport& operator=(const EnetTransport&) = delete;
    EnetTransport(EnetTransport&&)                 = delete;
    EnetTransport& operator=(EnetTransport&&)      = delete;

    // Host mode: bind a listener on `port`.
    // Returns true on success. Fills `error` on failure.
    bool listen(std::uint16_t port, std::string& error);

    // Client mode: connect to `host_str:port`. Local port is OS-chosen.
    bool connect(const std::string& host_str, std::uint16_t port,
                 std::string& error);

    // Client mode with a fixed local port (used after relay hole-punch
    // to preserve the NAT mapping). local_port=0 → OS chooses.
    bool connect_bound(const std::string& host_str, std::uint16_t port,
                       std::uint16_t local_port, std::string& error);

    // Client mode (relay path, phase 1): bind a UDP socket on
    // `local_port` (0 = OS-chosen) WITHOUT connecting to any peer yet.
    // The RelayClient uses the bound socket for the hole-punch; once the
    // hole is open, call connect_to_peer() to attach the ENet peer.
    //
    // Why this exists: enet_host_create() owns its socket internally, so
    // the only way to make the relay hole-punch and the ENet traffic
    // share a socket (and therefore share a NAT mapping) is to create
    // the ENet host BEFORE the relay handshake and hand its socket to
    // the RelayClient. See udp_socket_fd() below.
    bool bind_only(std::uint16_t local_port, std::string& error);

    // Client mode (relay path, phase 2): connect to `host_str:port`
    // using an ENet host previously created via bind_only(). The peer
    // address must already be hole-punched (the RelayClient's NullMsg
    // probes have opened the NAT in both directions).
    bool connect_to_peer(const std::string& host_str, std::uint16_t port,
                         std::string& error);

    // Returns the underlying UDP socket fd owned by the ENet host, or -1
    // if no host has been created yet. The fd is owned by this transport
    // — callers MUST NOT close it. Used by RelayClient to reuse the
    // same socket for the hole-punch, preserving the NAT mapping
    // established by the relay's UdpData packets.
    int udp_socket_fd() const;

    // ---- Relay intercept ----
    //
    // When the relay client reuses this transport's UDP socket
    // (udp_socket_fd()), ENet is the SOLE reader of that socket — the
    // relay client only sendto()s. So that ENet still feeds relay probe
    // packets (the peer's 1-byte NullMsg) back to the relay client, we
    // install ENet's `intercept` callback. On every packet ENet reads,
    // the intercept asks the sink "is this yours?"; if the sink returns
    // true (consumed), ENet skips it, otherwise ENet processes it
    // normally (the peer's real ENet CONNECT gets through).
    //
    // Without this, the relay client's own recvfrom() loop would race
    // with ENet for the shared socket and steal the peer's CONNECT.
    //
    // Abstract so enet_transport.hpp doesn't need to include
    // relay_client.hpp.
    class RelayPacketSink {
    public:
        virtual ~RelayPacketSink() = default;
        // See RelayClient::inject_received_packet. ip_nbo is the sender's
        // IPv4 in network byte order; port_hbo is host byte order.
        virtual bool inject_received_packet(const std::uint8_t* data,
                                            std::size_t len,
                                            std::uint32_t ip_nbo,
                                            std::uint16_t port_hbo) = 0;
    };

    // Attach the relay sink (before install_intercept). Owns nothing —
    // caller (NetplaySession) owns the RelayClient.
    void set_relay_sink(RelayPacketSink* sink) { relay_sink_ = sink; }

    // Wire the sink into ENet's intercept callback. No-op if host_ is
    // null or no sink is set. Call after both the host is created and
    // set_relay_sink() has been called.
    void install_intercept();

    // ENet intercept dispatch (public only so the C-linkage thunk in the
    // .cpp can call it — not part of the public API). Reads the packet
    // ENet just received, asks the sink whether it's a relay packet, and
    // returns 1 = consumed / 0 = let ENet process.
    static int dispatch_intercept(EnetTransport* self, _ENetHost* host);

    // Send a reliable packet on channel 0. Returns false if no peer
    // connected or send failed.
    bool send_reliable(const void* data, std::size_t size);

    // Send an unreliable packet on channel 1. Returns false on failure.
    bool send_unreliable(const void* data, std::size_t size);

    // Poll for events. Returns true if an event was dispatched (in `out`),
    // false if no event within `timeout_ms`.
    bool poll(std::uint32_t timeout_ms, TransportEvent& out);

    // Send a keep-alive ping to the peer. No-op if not connected.
    void ping();

    // Read the most recent received message (valid until next poll()).
    std::string_view last_message() const {
        return std::string_view(last_message_, last_message_len_);
    }

    // Get RTT/jitter/packet-loss stats from the peer. Returns zeros if
    // not connected.
    TransportStats get_stats() const;

    // True if we have a connected peer.
    bool is_connected() const { return connected_; }

    // True while the relay intercept is installed (relay sink set). Callers
    // use this to decide whether to pump poll() to feed the intercept even
    // when no ENet peer is connected yet (the relay hole-punch phase).
    bool is_shared_socket() const { return relay_sink_ != nullptr; }

    // Tear down everything. Idempotent.
    void deinit();

private:
    bool ensure_enet_initialized(std::string& error);

    _ENetHost*    host_              = nullptr;
    _ENetPeer*    peer_              = nullptr;
    bool          is_host_           = false;
    bool          connected_         = false;
    bool          owns_enet_init_    = false;

    RelayPacketSink* relay_sink_     = nullptr;

    static constexpr std::size_t kLastMessageCap = 4096;
    char          last_message_[kLastMessageCap] = {0};
    std::size_t   last_message_len_  = 0;
};

} // namespace caster::common::net
