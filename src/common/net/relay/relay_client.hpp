// src/common/net/relay/relay_client.hpp
//
// State machine for NAT-traversal relay handshake. TCP signaling + UDP
// hole-punch, all non-blocking, driven by step().
//
// Ported from zzcaster's `src/net/relay_client.zig`.
//
// Lifecycle:
//   RelayClientInit init{ relay, role, local_port, peer_id };
//   RelayClient client(init);
//   while (running) {
//       StepResult r = client.step();
//       if (std::holds_alternative<RelayResult>(r)) {
//           // Success — r.peer_ip, r.peer_port, r.local_udp_port filled.
//           break;
//       }
//       if (std::holds_alternative<RelayError>(r)) {
//           // Failure — r is the error code.
//           break;
//       }
//       // else: still in_progress
//   }
//   client.deinit();
//
// Threading: not thread-safe. Caller drives step() from one thread.

#pragma once

#include "relay_config.hpp"
#include "relay_protocol.hpp"
#include "../enet_transport.hpp"  // for EnetTransport::RelayPacketSink

#include <cstdint>
#include <optional>
#include <string>
#include <variant>

// Forward decls — we don't want to leak Win32 sockets into this header.
struct sockaddr_in;

namespace caster::common::net::relay_client {

enum class ClientRole {
    Host,
    Client,
};

enum class RelayError {
    TcpConnectFailed,
    TcpTimeout,
    RelayError,
    RelayDisconnected,
    MatchInfoTimeout,
    TunInfoTimeout,
    HolePunchFailed,
    InvalidRoomCode,    // terminal — not retried
    SocketError,
};

// Result of a one-shot room-code validation probe (see validate_room_code).
enum class RoomValidationResult {
    Valid,            // room exists, host is waiting
    NotFound,         // room not registered (host disconnected / never existed)
    Expired,          // room existed but expired
    NetworkError,     // couldn't reach the relay server
    InvalidCode,      // malformed code (wrong length / bad chars)
};

// One-shot validation: open a TCP connection to the relay, send ClientJoin,
// read the first server message, and classify the result. Does NOT proceed
// with the full handshake — closes the socket immediately after the probe.
//
// This is used by the GUI to validate a room code BEFORE starting the full
// relay join flow, so the user gets immediate feedback if the room doesn't
// exist (instead of waiting 60s for MatchInfoTimeout).
//
// timeout_ms defaults to 5000 (matches kTcpConnectTimeoutMs).
RoomValidationResult validate_room_code(const relay_config::RelayEntry& relay,
                                          std::string_view code,
                                          std::int64_t timeout_ms = 5000);

// Human-readable label for a RoomValidationResult.
const char* room_validation_label(RoomValidationResult r);

// Human-readable suggestion for what to do next.
const char* room_validation_suggestion(RoomValidationResult r);

// Returns a short human-readable label for the error.
const char* error_label(RelayError e);

// Returns a suggestion for what to do next.
const char* error_suggestion(RelayError e);

struct RelayResult {
    std::uint8_t peer_ip[4];   // raw bytes (not NBO)
    std::uint16_t peer_port;   // host byte order
    std::uint16_t local_udp_port;
};

struct RelayClientInit {
    relay_config::RelayEntry relay;
    ClientRole               role;
    std::uint16_t            local_port = 0;  // host: port to host on; client: 0 = OS
    std::string              peer_identifier;  // client: 4-letter room code; host: ignored

    // Pre-existing UDP socket to reuse for the hole-punch. -1 (default)
    // means RelayClient creates and owns its own socket.
    //
    // When set, RelayClient does NOT close the socket on deinit/restart/
    // fail — ownership stays with the caller (typically EnetTransport).
    // This is the fix for the "peer never arrives" bug: by reusing the
    // ENet host's socket, the relay learns the SAME public endpoint that
    // ENet will later use for game traffic, so the TunInfo the peer
    // receives points at a live NAT mapping instead of a dead one.
    int                      external_udp_socket = -1;
};

enum class RelayState {
    Idle,
    TcpConnecting,
    WaitingForHosted,      // host only
    WaitingForMatchInfo,   // both
    WaitingForTunInfo,     // both (sending UdpData every 50ms)
    HolePunching,          // both (sending UdpData + NullMsg every 50ms)
    Connected,             // terminal success
    Failed,                // terminal error
    Retrying,              // backoff, then restartHandshake
};

// Result of step(): in_progress (void), success (RelayResult), or failed (RelayError).
struct InProgress {};
using StepResult = std::variant<InProgress, RelayResult, RelayError>;

class RelayClient : public common::net::EnetTransport::RelayPacketSink {
public:
    explicit RelayClient(const RelayClientInit& init);
    ~RelayClient();

    RelayClient(const RelayClient&)            = delete;
    RelayClient& operator=(const RelayClient&) = delete;
    RelayClient(RelayClient&&)                 = delete;
    RelayClient& operator=(RelayClient&&)      = delete;

    // Drive the state machine. Call this every frame (or every ~10ms).
    StepResult step();

    // Cleanup. Idempotent.
    void deinit();

    // Cancel — next step() will transition to Failed.
    void cancel() { cancel_requested_ = true; }

    // Getters for UI display.
    RelayState                state()        const { return state_; }
    std::optional<RelayError> error()        const { return error_; }
    std::uint32_t             retry_count()  const { return retry_count_;}
    std::optional<std::string> get_room_code() const;

    // True when RelayClient is reusing an externally-owned UDP socket
    // (EnetTransport's). In that mode RelayClient only sendto()s and MUST
    // NOT recvfrom() the socket — ENet is the sole reader and feeds
    // inbound packets back via inject_received_packet() (through ENet's
    // intercept callback). See EnetTransport::install_intercept().
    bool wants_socket_send_only() const { return external_udp_socket_ != -1; }

    // Feed a packet that ENet's intercept callback pulled off the shared
    // socket. sender_ip_nbo is the sender's IPv4 in network byte order
    // (as ENet/Winsock report it); sender_port_hbo is host byte order.
    //
    // Returns true if the packet was a relay protocol packet (NullMsg
    // from the peer, or UdpData echo from the relay) and was consumed;
    // the intercept callback then tells ENet to skip it. Returns false
    // for anything else (e.g. the peer's real ENet CONNECT), letting
    // ENet process it normally.
    //
    // On the matching peer packet during HolePunching, transitions the
    // state machine to Connected — this replaces the old recvfrom()
    // drain loop that competed with ENet for the socket.
    bool inject_received_packet(const std::uint8_t* data, std::size_t len,
                                 std::uint32_t sender_ip_nbo,
                                 std::uint16_t sender_port_hbo) override;

private:
    // Internal helpers (mirror zzcaster).
    void restart_handshake();
    void send_initial_message();
    void open_udp_socket();
    void send_udp_data();
    void send_null_msg();
    bool try_read_tcp();
    bool try_parse_server_msg();
    void fail(RelayError err);
    void step_retrying();

    // Config (immutable post-init).
    relay_config::RelayEntry relay_;
    ClientRole               role_;
    std::uint16_t            local_port_ = 0;
    char                     room_code_[4] = {0};
    bool                     room_code_set_ = false;

    // Sockets.
    int tcp_sock_ = -1;  // INVALID_SOCKET
    int udp_sock_ = -1;

    // External UDP socket (from EnetTransport). When non-negative, RelayClient
    // reuses this fd for UdpData / NullMsg and does NOT close it. Owned by
    // the caller for the entire lifetime of the RelayClient.
    int  external_udp_socket_ = -1;
    bool owns_udp_socket_     = true;  // false when external_udp_socket_ is used

    // TCP read buffer (TCP doesn't preserve message boundaries).
    static constexpr std::size_t kTcpBufSize = 256;
    std::uint8_t tcp_read_buf_[kTcpBufSize] = {0};
    std::size_t  tcp_read_pos_ = 0;

    // UDP state.
    std::uint16_t local_udp_port_ = 0;
    sockaddr_in*  relay_udp_addr_ = nullptr;  // owned (heap-allocated)
    sockaddr_in*  peer_addr_      = nullptr;  // owned, set in TunInfo

    // Match state.
    std::uint32_t match_id_ = 0;

    // Timers (wall-clock ms).
    std::int64_t phase_start_ms_    = 0;
    std::int64_t last_udp_data_ms_  = 0;
    std::int64_t last_null_msg_ms_  = 0;

    // State.
    RelayState                state_      = RelayState::Idle;
    std::optional<RelayError> error_;

    // Retry.
    std::uint32_t retry_count_  = 0;
    std::int64_t  next_retry_ms_ = 0;

    // Cached current time during step().
    std::int64_t current_ms_ = 0;

    bool cancel_requested_ = false;
};

} // namespace caster::common::net::relay_client
