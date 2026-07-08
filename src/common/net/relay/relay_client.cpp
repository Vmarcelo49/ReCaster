// src/common/net/relay/relay_client.cpp

#include "relay_client.hpp"
#include "relay_protocol.hpp"
#include "../../logger.hpp"

#ifndef NOMINMAX
#  define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <chrono>
#include <cstring>
#include <new>

namespace caster::common::net::relay_client {

namespace {

namespace rp = relay_protocol;

constexpr int kInvalidSocket = -1;

// Timeouts (ms).
constexpr std::int64_t kTcpConnectTimeoutMs  = 5000;
constexpr std::int64_t kHostedTimeoutMs      = 5000;
constexpr std::int64_t kMatchInfoTimeoutMs   = 60000;
constexpr std::int64_t kTunInfoTimeoutMs     = 10000;
constexpr std::int64_t kHolePunchTimeoutMs   = 10000;
constexpr std::int64_t kRetryInitialDelayMs  = 1000;
constexpr std::int64_t kRetryMaxDelayMs      = 5000;
constexpr std::int64_t kUdpDataIntervalMs    = 50;
constexpr std::int64_t kNullMsgIntervalMs    = 50;

constexpr std::size_t kUdpBufSize = 64;

std::int64_t now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(
        steady_clock::now().time_since_epoch()).count();
}

std::int64_t retry_delay_ms(std::uint32_t attempt) {
    // Exponential backoff: 1s, 2s, 4s, 8s, ... capped at 5s.
    std::uint32_t shift = std::min<std::uint32_t>(attempt - 1, 30);
    std::int64_t raw = kRetryInitialDelayMs << shift;
    return std::min(raw, kRetryMaxDelayMs);
}

bool is_retriable(RelayError e) {
    return e != RelayError::InvalidRoomCode;
}

void set_non_blocking(int sock, bool enable) {
    u_long mode = enable ? 1 : 0;
    ioctlsocket(sock, FIONBIO, &mode);
}

// Resolve a hostname to a 32-bit IPv4 address in network byte order.
// Returns 0 on failure. Tries inet_addr first, then gethostbyname.
std::uint32_t resolve_host(const std::string& host) {
    std::uint32_t addr = inet_addr(host.c_str());
    if (addr != INADDR_NONE && addr != 0) return addr;

    struct hostent* he = gethostbyname(host.c_str());
    if (!he || !he->h_addr_list[0]) return 0;
    std::uint32_t out;
    std::memcpy(&out, he->h_addr_list[0], 4);
    return out;
}

// Parse "ip:port" string into {ip NBO, port HBO}. Returns 0 ip on failure.
struct IpPort {
    std::uint32_t ip_nbo;
    std::uint16_t port_hbo;
};
IpPort parse_ip_port(const std::string& s) {
    IpPort out{0, 0};
    auto colon = s.rfind(':');
    if (colon == std::string::npos) return out;
    std::string ip_str = s.substr(0, colon);
    std::string port_str = s.substr(colon + 1);
    out.ip_nbo = inet_addr(ip_str.c_str());
    if (out.ip_nbo == INADDR_NONE || out.ip_nbo == 0) return out;
    try {
        int p = std::stoi(port_str);
        if (p < 1 || p > 65535) return out;
        out.port_hbo = static_cast<std::uint16_t>(p);
    } catch (...) {
        return out;
    }
    return out;
}

} // namespace

const char* error_label(RelayError e) {
    switch (e) {
        case RelayError::TcpConnectFailed:   return "Relay unreachable";
        case RelayError::TcpTimeout:         return "Relay did not respond";
        case RelayError::RelayError:         return "Relay server error";
        case RelayError::RelayDisconnected:  return "Relay disconnected";
        case RelayError::MatchInfoTimeout:   return "No opponent joined (60s timeout)";
        case RelayError::TunInfoTimeout:     return "Connection negotiation timed out";
        case RelayError::HolePunchFailed:    return "Hole-punch failed (NAT too restrictive?)";
        case RelayError::InvalidRoomCode:    return "Invalid room code";
        case RelayError::SocketError:        return "Network error";
    }
    return "Unknown error";
}

const char* error_suggestion(RelayError e) {
    switch (e) {
        case RelayError::TcpConnectFailed:
            return "Check your internet connection or try a different relay.";
        case RelayError::TcpTimeout:
            return "Relay server may be down. Try again later.";
        case RelayError::RelayError:
            return "The relay rejected the request. Verify the room code.";
        case RelayError::RelayDisconnected:
            return "Relay server closed the connection. Try again.";
        case RelayError::MatchInfoTimeout:
            return "Ask your opponent to double-check the room code.";
        case RelayError::TunInfoTimeout:
            return "Network negotiation failed. Try a direct connection instead.";
        case RelayError::HolePunchFailed:
            return "Your NAT may be too restrictive. Try port-forwarding or a different network.";
        case RelayError::InvalidRoomCode:
            return "Room codes are 4 letters/digits (no I, O, 0, 1).";
        case RelayError::SocketError:
            return "Check your network connection and try again.";
    }
    return "Try again or use a direct connection.";
}

RelayClient::RelayClient(const RelayClientInit& init)
    : relay_(init.relay)
    , role_(init.role)
    , local_port_(init.local_port)
    , external_udp_socket_(init.external_udp_socket)
    , owns_udp_socket_(init.external_udp_socket == -1) {

    if (role_ == ClientRole::Client) {
        // Validate room code.
        if (!rp::is_valid_room_code(init.peer_identifier)) {
            state_ = RelayState::Failed;
            error_ = RelayError::InvalidRoomCode;
            return;
        }
        std::memcpy(room_code_, init.peer_identifier.data(), 4);
        room_code_set_ = true;
    }
    // Host generates room code in restart_handshake (using a fresh seed).

    restart_handshake();
}

RelayClient::~RelayClient() {
    deinit();
}

void RelayClient::deinit() {
    if (tcp_sock_ != kInvalidSocket) {
        closesocket(tcp_sock_);
        tcp_sock_ = kInvalidSocket;
    }
    // Only close the UDP socket if we own it. When an external socket was
    // supplied (EnetTransport reuse path), the caller retains ownership.
    if (udp_sock_ != kInvalidSocket && owns_udp_socket_) {
        closesocket(udp_sock_);
    }
    udp_sock_ = kInvalidSocket;
    delete relay_udp_addr_;
    relay_udp_addr_ = nullptr;
    delete peer_addr_;
    peer_addr_ = nullptr;
}

void RelayClient::restart_handshake() {
    // Reset mutable state.
    if (tcp_sock_ != kInvalidSocket) {
        closesocket(tcp_sock_);
        tcp_sock_ = kInvalidSocket;
    }
    // Tear down our own UDP socket on retry. An externally-supplied socket
    // (EnetTransport reuse path) survives the restart — we just clear our
    // local reference; open_udp_socket() will re-adopt it on the next cycle.
    if (udp_sock_ != kInvalidSocket && owns_udp_socket_) {
        closesocket(udp_sock_);
    }
    udp_sock_ = kInvalidSocket;
    delete relay_udp_addr_;
    relay_udp_addr_ = nullptr;
    delete peer_addr_;
    peer_addr_ = nullptr;

    tcp_read_pos_ = 0;
    local_udp_port_ = 0;
    match_id_ = 0;
    error_.reset();

    current_ms_ = now_ms();

    // For host: generate a fresh room code (preserved across retries —
    // wait, zzcaster preserves it. Let me check... actually zzcaster
    // generates the room code ONCE in init() and preserves it across
    // retries. So only generate if not yet set.)
    if (role_ == ClientRole::Host && !room_code_set_) {
        // Use a high-res clock seed.
        std::uint64_t seed = static_cast<std::uint64_t>(current_ms_) ^
                             (static_cast<std::uint64_t>(GetCurrentProcessId()) << 32);
        std::string code = rp::generate_room_code(seed);
        std::memcpy(room_code_, code.data(), 4);
        room_code_set_ = true;
        logger::info("relay_client: generated room code {}", code);
    }

    // Resolve relay host.
    std::uint32_t relay_ip = resolve_host(relay_.host);
    if (relay_ip == 0) {
        fail(RelayError::TcpConnectFailed);
        return;
    }

    // Create TCP socket, non-blocking, connect.
    tcp_sock_ = static_cast<int>(socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
    if (tcp_sock_ == INVALID_SOCKET) {
        fail(RelayError::SocketError);
        return;
    }
    set_non_blocking(tcp_sock_, true);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(relay_.port);
    addr.sin_addr.s_addr = relay_ip;
    int rc = connect(tcp_sock_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    if (rc == 0) {
        // Immediate success (rare for non-blocking). Send initial message.
        send_initial_message();
        return;
    }
    int err = WSAGetLastError();
    if (err != WSAEWOULDBLOCK) {
        fail(RelayError::TcpConnectFailed);
        return;
    }

    state_ = RelayState::TcpConnecting;
    phase_start_ms_ = current_ms_;
}

void RelayClient::send_initial_message() {
    char buf[64];
    std::size_t n;
    if (role_ == ClientRole::Host) {
        std::string_view code(room_code_, 4);
        n = rp::encode_host_register(buf, sizeof(buf), rp::kTypeUdp,
                                       local_port_, code);
    } else {
        std::string_view code(room_code_, 4);
        n = rp::encode_client_join(buf, sizeof(buf), rp::kTypeUdp, code);
    }
    if (n == 0) {
        fail(RelayError::SocketError);
        return;
    }
    int sent = send(tcp_sock_, buf, static_cast<int>(n), 0);
    if (sent != static_cast<int>(n)) {
        fail(RelayError::SocketError);
        return;
    }

    if (role_ == ClientRole::Host) {
        state_ = RelayState::WaitingForHosted;
    } else {
        state_ = RelayState::WaitingForMatchInfo;
    }
    phase_start_ms_ = current_ms_;
}

void RelayClient::open_udp_socket() {
    if (external_udp_socket_ != -1) {
        // Reuse the caller-supplied UDP socket (typically the ENet host's
        // socket). This is the key fix: the relay's UdpData packets and
        // the subsequent ENet game traffic share a single NAT mapping,
        // so the TunInfo the peer receives points at a live endpoint.
        udp_sock_ = external_udp_socket_;
        owns_udp_socket_ = false;

        // Make sure it's non-blocking (ENet already sets this, but our
        // recvfrom loop in HolePunching assumes non-blocking — idempotent).
        set_non_blocking(udp_sock_, true);

        // Discover the actual port we bound to (for UI display + HostRegister).
        sockaddr_in actual{};
        int actual_len = sizeof(actual);
        if (getsockname(udp_sock_, reinterpret_cast<sockaddr*>(&actual),
                         &actual_len) == 0) {
            local_udp_port_ = ntohs(actual.sin_port);
        }

        // Cache the relay's UDP endpoint (same host as TCP, same port).
        delete relay_udp_addr_;
        relay_udp_addr_ = new sockaddr_in();
        std::uint32_t relay_ip = resolve_host(relay_.host);
        relay_udp_addr_->sin_family = AF_INET;
        relay_udp_addr_->sin_port = htons(relay_.port);
        relay_udp_addr_->sin_addr.s_addr = relay_ip;
        return;
    }

    udp_sock_ = static_cast<int>(socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP));
    if (udp_sock_ == INVALID_SOCKET) {
        fail(RelayError::SocketError);
        return;
    }

    // Allow rebinding to the same port (important for relay handoff).
    BOOL reuse = TRUE;
    setsockopt(udp_sock_, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&reuse), sizeof(reuse));

    sockaddr_in bind_addr{};
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_port = htons(local_port_);  // 0 = OS chooses
    bind_addr.sin_addr.s_addr = INADDR_ANY;
    if (bind(udp_sock_, reinterpret_cast<sockaddr*>(&bind_addr),
              sizeof(bind_addr)) != 0) {
        fail(RelayError::SocketError);
        return;
    }

    set_non_blocking(udp_sock_, true);

    // Discover the actual port we bound to.
    sockaddr_in actual{};
    int actual_len = sizeof(actual);
    if (getsockname(udp_sock_, reinterpret_cast<sockaddr*>(&actual),
                     &actual_len) == 0) {
        local_udp_port_ = ntohs(actual.sin_port);
    }

    // Cache the relay UDP endpoint.
    delete relay_udp_addr_;
    relay_udp_addr_ = new sockaddr_in();
    // Relay uses the same host as TCP but on the same port (zzcaster convention).
    // Actually, the relay sends UDP from its TCP port. So relay_udp_addr_ =
    // {relay.host, relay.port}.
    std::uint32_t relay_ip = resolve_host(relay_.host);
    relay_udp_addr_->sin_family = AF_INET;
    relay_udp_addr_->sin_port = htons(relay_.port);
    relay_udp_addr_->sin_addr.s_addr = relay_ip;
}

void RelayClient::send_udp_data() {
    if (!relay_udp_addr_ || udp_sock_ == kInvalidSocket) return;
    char buf[8];
    std::size_t n = rp::encode_udp_data(buf, sizeof(buf),
                                         role_ == ClientRole::Client,
                                         match_id_);
    if (n == 0) return;
    sendto(udp_sock_, buf, static_cast<int>(n), 0,
           reinterpret_cast<sockaddr*>(relay_udp_addr_),
           sizeof(*relay_udp_addr_));
    last_udp_data_ms_ = current_ms_;
}

void RelayClient::send_null_msg() {
    if (!peer_addr_ || udp_sock_ == kInvalidSocket) return;
    char null_msg[1] = {0x00};
    sendto(udp_sock_, null_msg, 1, 0,
           reinterpret_cast<sockaddr*>(peer_addr_),
           sizeof(*peer_addr_));
    last_null_msg_ms_ = current_ms_;
}

bool RelayClient::try_read_tcp() {
    if (tcp_sock_ == kInvalidSocket) return false;

    int got = recv(tcp_sock_,
                    reinterpret_cast<char*>(tcp_read_buf_ + tcp_read_pos_),
                    static_cast<int>(kTcpBufSize - tcp_read_pos_), 0);
    if (got > 0) {
        tcp_read_pos_ += got;
        return true;
    }
    if (got == 0) {
        // Connection closed by relay.
        fail(RelayError::RelayDisconnected);
        return false;
    }
    int err = WSAGetLastError();
    if (err == WSAEWOULDBLOCK) {
        return true;  // open, no data
    }
    fail(RelayError::SocketError);
    return false;
}

bool RelayClient::try_parse_server_msg() {
    if (tcp_read_pos_ == 0) return false;

    rp::ServerMsg msg = rp::decode_server_msg(tcp_read_buf_, tcp_read_pos_);
    if (msg.kind == rp::ServerMsgKind::Unknown) {
        return false;  // need more bytes
    }

    std::size_t consumed = rp::consumed_bytes(msg, tcp_read_pos_);
    if (consumed == 0 || consumed > tcp_read_pos_) return false;

    // Shift the buffer.
    std::memmove(tcp_read_buf_, tcp_read_buf_ + consumed,
                  tcp_read_pos_ - consumed);
    tcp_read_pos_ -= consumed;

    // Dispatch.
    switch (msg.kind) {
        case rp::ServerMsgKind::Hosted:
            // Host: relay confirms/assigns the room code.
            if (role_ == ClientRole::Host) {
                std::memcpy(room_code_, msg.hosted.code.data(), 4);
                room_code_set_ = true;
                logger::info("relay_client: room code confirmed: {}",
                             msg.hosted.code);
                state_ = RelayState::WaitingForMatchInfo;
                phase_start_ms_ = current_ms_;
            }
            break;

        case rp::ServerMsgKind::MatchInfo:
            match_id_ = msg.match_info.match_id;
            logger::info("relay_client: match_id={}", match_id_);
            open_udp_socket();
            if (state_ != RelayState::Failed) {
                state_ = RelayState::WaitingForTunInfo;
                phase_start_ms_ = current_ms_;
            }
            break;

        case rp::ServerMsgKind::TunInfo: {
            IpPort pp = parse_ip_port(msg.tun_info.addr);
            if (pp.ip_nbo == 0) {
                fail(RelayError::RelayError);
                break;
            }
            delete peer_addr_;
            peer_addr_ = new sockaddr_in();
            peer_addr_->sin_family = AF_INET;
            peer_addr_->sin_port = htons(pp.port_hbo);
            peer_addr_->sin_addr.s_addr = pp.ip_nbo;
            logger::info("relay_client: peer endpoint = {}",
                         msg.tun_info.addr);
            state_ = RelayState::HolePunching;
            phase_start_ms_ = current_ms_;
            break;
        }

        case rp::ServerMsgKind::Error:
            logger::warn("relay_client: server error code={} msg='{}'",
                         msg.error.code, msg.error.msg);
            fail(RelayError::RelayError);
            break;

        default:
            break;
    }
    return true;
}

void RelayClient::fail(RelayError err) {
    error_ = err;
    if (!is_retriable(err)) {
        state_ = RelayState::Failed;
        return;
    }
    // Retriable: tear down sockets, schedule retry.
    if (tcp_sock_ != kInvalidSocket) {
        closesocket(tcp_sock_);
        tcp_sock_ = kInvalidSocket;
    }
    // Only close the UDP socket if we own it. External sockets (EnetTransport
    // reuse path) outlive this RelayClient and must remain open so the caller
    // can reuse them on the next attempt or for the post-relay ENet phase.
    if (udp_sock_ != kInvalidSocket && owns_udp_socket_) {
        closesocket(udp_sock_);
    }
    udp_sock_ = kInvalidSocket;
    delete relay_udp_addr_;
    relay_udp_addr_ = nullptr;
    delete peer_addr_;
    peer_addr_ = nullptr;

    retry_count_++;
    next_retry_ms_ = current_ms_ + retry_delay_ms(retry_count_);
    state_ = RelayState::Retrying;
    logger::warn("relay_client: {} (retry {} in {} ms)",
                 error_label(err), retry_count_,
                 retry_delay_ms(retry_count_));
}

void RelayClient::step_retrying() {
    if (current_ms_ < next_retry_ms_) return;
    restart_handshake();
}

StepResult RelayClient::step() {
    if (cancel_requested_ && state_ != RelayState::Failed) {
        state_ = RelayState::Failed;
        error_ = RelayError::RelayError;
    }

    if (state_ == RelayState::Connected) {
        // Build the result.
        RelayResult r;
        if (peer_addr_) {
            std::memcpy(r.peer_ip, &peer_addr_->sin_addr.s_addr, 4);
            r.peer_port = ntohs(peer_addr_->sin_port);
        }
        r.local_udp_port = local_udp_port_;
        return r;
    }
    if (state_ == RelayState::Failed) {
        return error_.value_or(RelayError::SocketError);
    }

    current_ms_ = now_ms();

    switch (state_) {
        case RelayState::Idle:
            restart_handshake();
            break;

        case RelayState::TcpConnecting: {
            if (current_ms_ - phase_start_ms_ > kTcpConnectTimeoutMs) {
                fail(RelayError::TcpConnectFailed);
                break;
            }
            // Check if socket is writable (connect completed).
            fd_set write_set, except_set;
            FD_ZERO(&write_set);
            FD_ZERO(&except_set);
            FD_SET(tcp_sock_, &write_set);
            FD_SET(tcp_sock_, &except_set);
            timeval tv{0, 0};
            int rc = select(0, nullptr, &write_set, &except_set, &tv);
            if (rc > 0) {
                if (FD_ISSET(tcp_sock_, &except_set)) {
                    fail(RelayError::TcpConnectFailed);
                } else if (FD_ISSET(tcp_sock_, &write_set)) {
                    // Check SO_ERROR to be sure.
                    int so_err = 0;
                    int so_len = sizeof(so_err);
                    getsockopt(tcp_sock_, SOL_SOCKET, SO_ERROR,
                               reinterpret_cast<char*>(&so_err), &so_len);
                    if (so_err != 0) {
                        fail(RelayError::TcpConnectFailed);
                    } else {
                        send_initial_message();
                    }
                }
            }
            break;
        }

        case RelayState::WaitingForHosted:
            if (current_ms_ - phase_start_ms_ > kHostedTimeoutMs) {
                fail(RelayError::TcpTimeout);
                break;
            }
            if (try_read_tcp()) {
                try_parse_server_msg();
            }
            break;

        case RelayState::WaitingForMatchInfo:
            if (current_ms_ - phase_start_ms_ > kMatchInfoTimeoutMs) {
                fail(RelayError::MatchInfoTimeout);
                break;
            }
            if (try_read_tcp()) {
                try_parse_server_msg();
            }
            break;

        case RelayState::WaitingForTunInfo:
            if (current_ms_ - phase_start_ms_ > kTunInfoTimeoutMs) {
                fail(RelayError::TunInfoTimeout);
                break;
            }
            if (try_read_tcp()) {
                try_parse_server_msg();
            }
            // Send UdpData to relay every 50ms.
            if (current_ms_ - last_udp_data_ms_ >= kUdpDataIntervalMs) {
                send_udp_data();
            }
            break;

        case RelayState::HolePunching: {
            if (current_ms_ - phase_start_ms_ > kHolePunchTimeoutMs) {
                fail(RelayError::HolePunchFailed);
                break;
            }
            // Keep sending UdpData (NAT keep-alive) + NullMsg (peer hole-punch).
            if (current_ms_ - last_udp_data_ms_ >= kUdpDataIntervalMs) {
                send_udp_data();
            }
            if (current_ms_ - last_null_msg_ms_ >= kNullMsgIntervalMs) {
                send_null_msg();
            }
            // Drain recvfrom in a loop — first packet from peer = success.
            std::uint8_t buf[kUdpBufSize];
            sockaddr_in from{};
            int from_len = sizeof(from);
            while (true) {
                int got = recvfrom(udp_sock_, reinterpret_cast<char*>(buf),
                                    sizeof(buf), 0,
                                    reinterpret_cast<sockaddr*>(&from),
                                    &from_len);
                if (got <= 0) break;  // WSAEWOULDBLOCK or error
                if (peer_addr_ &&
                    from.sin_addr.s_addr == peer_addr_->sin_addr.s_addr &&
                    from.sin_port == peer_addr_->sin_port) {
                    // Success — peer reached us.
                    logger::info("relay_client: hole-punch succeeded");
                    state_ = RelayState::Connected;
                    break;
                }
                // Packet from unknown source — ignore.
            }
            break;
        }

        case RelayState::Retrying:
            step_retrying();
            break;

        default:
            break;
    }

    // Re-check terminal states after step.
    if (state_ == RelayState::Connected) {
        RelayResult r;
        if (peer_addr_) {
            std::memcpy(r.peer_ip, &peer_addr_->sin_addr.s_addr, 4);
            r.peer_port = ntohs(peer_addr_->sin_port);
        }
        r.local_udp_port = local_udp_port_;
        return r;
    }
    if (state_ == RelayState::Failed) {
        return error_.value_or(RelayError::SocketError);
    }
    return InProgress{};
}

std::optional<std::string> RelayClient::get_room_code() const {
    if (!room_code_set_) return std::nullopt;
    return std::string(room_code_, 4);
}

} // namespace caster::common::net::relay_client
