// src/exe/session/session.cpp

#include "session.hpp"
#include "netplay_config.hpp"
#include "../../common/net/connection_type.hpp"
#include "../../common/net/ip_discovery.hpp"
#include "../../common/net/relay/relay_client.hpp"
#include "../../common/net/relay/relay_config.hpp"
#include "../../common/net/relay/relay_protocol.hpp"
#include "../../common/net/connection_detector.hpp"
#include "../../common/logger.hpp"
#include "../../common/win32/window.hpp"

#ifndef NOMINMAX
#  define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <windows.h>

#include <chrono>
#include <cmath>
#include <cstring>
#include <string>

namespace caster::exe::session {

namespace {

namespace rp = common::net::relay_protocol;
namespace rc = common::net::relay_config;
namespace rclient = common::net::relay_client;
namespace cd = common::net::connection_detector;

std::int64_t now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(
        steady_clock::now().time_since_epoch()).count();
}

// Timeouts (ms).
constexpr std::int64_t kListenTimeoutMs          = 3'600'000;  // 1h
constexpr std::int64_t kConnectTimeoutMs         = 30'000;
constexpr std::int64_t kVersionTimeoutMs         = 5'000;
constexpr std::int64_t kNameTimeoutMs            = 5'000;
constexpr std::int64_t kHostWaitConfirmTimeoutMs = 30'000;
constexpr std::int64_t kPingPerAttemptTimeoutMs  = 833;
constexpr std::int64_t kHeartbeatIntervalMs      = 2'000;

// Handshake wire protocol message tags.
enum class MsgTag : std::uint8_t {
    Version = 1,
    Config  = 2,
    Confirm = 3,
    Ping    = 4,
    Name    = 6,
};

} // namespace

NetplaySession::NetplaySession() = default;

NetplaySession::~NetplaySession() {
    deinit();
}

void NetplaySession::deinit() {
    // Tear down relay first (releases UDP port) then transport.
    relay_client_.reset();
    relay_list_.clear();
    transport_.deinit();
}

// ---- Start methods ------------------------------------------------------

bool NetplaySession::start_host(std::uint16_t port, bool training) {
    std::string err;
    if (!transport_.listen(port, err)) {
        set_error(err);
        state_ = SessionState::Failed;
        return false;
    }
    config_.is_host = true;
    config_.is_training = training;
    config_.is_netplay = true;
    config_.host_player = 1;
    config_.local_player = 1;
    config_.remote_player = 2;
    state_ = SessionState::Listening;
    set_phase_timeout(kListenTimeoutMs);
    set_status("Listening for direct connection...");
    return true;
}

bool NetplaySession::start_join(const std::string& host_str,
                                 std::uint16_t port, bool training) {
    std::string err;
    if (!transport_.connect(host_str, port, err)) {
        set_error(err);
        state_ = SessionState::Failed;
        return false;
    }
    config_.is_host = false;
    config_.is_training = training;
    config_.is_netplay = true;
    config_.host_player = 2;  // host is player 1
    config_.local_player = 2;
    config_.remote_player = 1;

    // Record peer address + port so the DLL's ENet transport can reconnect
    // after the launcher's transport is torn down (session.deinit()).
    // local_udp_port=0 lets the OS choose the client's bind port (no need
    // to match the host's port — ENet connects outbound).
    config_.peer_addr      = host_str;
    config_.peer_port      = port;
    config_.local_udp_port = 0;

    state_ = SessionState::Connecting;
    set_phase_timeout(kConnectTimeoutMs);
    set_status("Connecting to host...");
    return true;
}

bool NetplaySession::start_smart_host(const std::string& relay_source,
                                       std::uint16_t port, bool training) {
    // 1. Start direct listener FIRST.
    std::string err;
    if (!transport_.listen(port, err)) {
        set_error(err);
        state_ = SessionState::Failed;
        return false;
    }
    config_.is_host = true;
    config_.is_training = training;
    config_.is_netplay = true;
    config_.host_player = 1;
    config_.local_player = 1;
    config_.remote_player = 2;

    // Record the listening port so the DLL's ENet transport can rebind to
    // the same port after the launcher's transport is torn down. The client
    // will connect to this port (peer_port = port for direct connections).
    // peer_addr stays empty on the host side — the host doesn't connect
    // outbound, it just listens and accepts the client's connection.
    config_.local_udp_port = port;
    config_.peer_port      = port;

    // 2. Parse relay list and start relay client in parallel.
    if (relay_source.empty()) {
        relay_list_ = rc::default_list();
    } else {
        relay_list_ = rc::parse_list(relay_source);
    }
    if (!relay_list_.empty()) {
        rclient::RelayClientInit init;
        init.relay = relay_list_[0];
        init.role = rclient::ClientRole::Host;
        init.local_port = 0;  // smart host: relay uses OS-chosen port
        relay_client_ = std::make_unique<rclient::RelayClient>(init);
    }

    state_ = SessionState::Listening;
    set_phase_timeout(kListenTimeoutMs);
    set_status("Listening for direct connection...");
    return true;
}

bool NetplaySession::start_smart_join(const std::string& input,
                                       const std::string& relay_source,
                                       bool training) {
    auto parsed = cd::parse_input(input);
    if (parsed.type == cd::InputType::RoomCode) {
        return start_relay_join(relay_source, parsed.room_code, training);
    }
    if (parsed.type == cd::InputType::IpPort) {
        return start_join(parsed.host, static_cast<std::uint16_t>(parsed.port),
                          training);
    }
    set_error("Invalid input. Use host:port or #room");
    state_ = SessionState::Failed;
    return false;
}

bool NetplaySession::start_relay_host(const std::string& relay_source,
                                       std::uint16_t port, bool training) {
    if (relay_source.empty()) {
        relay_list_ = rc::default_list();
    } else {
        relay_list_ = rc::parse_list(relay_source);
    }
    if (relay_list_.empty()) {
        set_error("No relay servers configured");
        state_ = SessionState::Failed;
        return false;
    }

    rclient::RelayClientInit init;
    init.relay = relay_list_[0];
    init.role = rclient::ClientRole::Host;
    init.local_port = port;
    relay_client_ = std::make_unique<rclient::RelayClient>(init);

    config_.is_host = true;
    config_.is_training = training;
    config_.is_netplay = true;
    config_.host_player = 1;
    config_.local_player = 1;
    config_.remote_player = 2;
    state_ = SessionState::RelayConnecting;
    set_phase_timeout(0);  // RelayClient manages its own timeouts
    set_status("Connecting to relay server...");
    return true;
}

bool NetplaySession::start_relay_join(const std::string& relay_source,
                                       const std::string& peer_identifier,
                                       bool training) {
    if (relay_source.empty()) {
        relay_list_ = rc::default_list();
    } else {
        relay_list_ = rc::parse_list(relay_source);
    }
    if (relay_list_.empty()) {
        set_error("No relay servers configured");
        state_ = SessionState::Failed;
        return false;
    }

    rclient::RelayClientInit init;
    init.relay = relay_list_[0];
    init.role = rclient::ClientRole::Client;
    init.local_port = 0;
    init.peer_identifier = peer_identifier;
    relay_client_ = std::make_unique<rclient::RelayClient>(init);

    config_.is_host = false;
    config_.is_training = training;
    config_.is_netplay = true;
    config_.host_player = 2;
    config_.local_player = 2;
    config_.remote_player = 1;
    state_ = SessionState::RelayConnecting;
    set_phase_timeout(0);
    set_status("Connecting to relay server...");
    return true;
}

// ---- Drive the state machine --------------------------------------------

void NetplaySession::step() {
    if (cancel_requested_ &&
        state_ != SessionState::Launching &&
        state_ != SessionState::Failed &&
        state_ != SessionState::Cancelled) {
        state_ = SessionState::Cancelled;
        return;
    }

    maybe_heartbeat();

    switch (state_) {
        case SessionState::Idle:
        case SessionState::Launching:
        case SessionState::Completed:
        case SessionState::Failed:
        case SessionState::Cancelled:
            return;
        case SessionState::WaitingConfirmation: {
            // Just poll for disconnect.
            common::net::TransportEvent ev;
            if (transport_.poll(0, ev) && ev == common::net::TransportEvent::Disconnected) {
                set_error("Peer disconnected while waiting");
                state_ = SessionState::Failed;
            }
            return;
        }
        case SessionState::Listening:
            step_listening();
            return;
        case SessionState::Connecting:
            step_connecting();
            return;
        case SessionState::Handshaking:
            step_handshaking();
            return;
        case SessionState::PingExchanging:
            step_ping_exchanging();
            return;
        case SessionState::RelayConnecting:
            step_relay();
            return;
    }
}

void NetplaySession::host_confirm() {
    if (state_ != SessionState::WaitingConfirmation) return;
    host_confirmed_ = true;
    send_config_message();
    handshake_subphase_ = 3;
    set_phase_timeout(kHostWaitConfirmTimeoutMs);
    state_ = SessionState::Handshaking;
}

// ---- Helpers ------------------------------------------------------------

void NetplaySession::set_phase_timeout(std::int64_t duration_ms) {
    if (duration_ms == 0) {
        phase_start_ms_ = 0;
        phase_deadline_ms_ = 0;
        return;
    }
    phase_start_ms_ = now_ms();
    phase_deadline_ms_ = phase_start_ms_ + duration_ms;
}

bool NetplaySession::phase_timed_out() const {
    if (phase_deadline_ms_ == 0) return false;
    return now_ms() >= phase_deadline_ms_;
}

void NetplaySession::set_error(const std::string& msg) {
    error_message_ = msg;
    common::logger::err("session: {}", msg);
}

void NetplaySession::set_status(const std::string& msg) {
    status_message_ = msg;
}

std::string NetplaySession::status_message() const {
    if (status_message_.empty()) return "Idle.";
    return status_message_;
}

std::optional<std::uint32_t> NetplaySession::remaining_seconds() const {
    if (phase_deadline_ms_ == 0) return std::nullopt;
    std::int64_t now = now_ms();
    if (now >= phase_deadline_ms_) return 0;
    return static_cast<std::uint32_t>((phase_deadline_ms_ - now) / 1000);
}

std::optional<std::string> NetplaySession::room_code() const {
    if (!relay_client_) return std::nullopt;
    return relay_client_->get_room_code();
}

void NetplaySession::maybe_heartbeat() {
    if (!transport_.is_connected()) return;
    std::int64_t now = now_ms();
    if (now - last_heartbeat_ms_ >= kHeartbeatIntervalMs) {
        transport_.ping();
        last_heartbeat_ms_ = now;
    }
}

void NetplaySession::lookup_host_addresses() {
    auto pub = common::net::ip_discovery::get_public_ip();
    if (!pub.empty()) public_ip_ = pub;
    auto loc = common::net::ip_discovery::get_local_ip();
    if (!loc.empty()) local_ip_ = loc;
}

void NetplaySession::detect_connection_type() {
    config_.local_connection_type = common::net::connection_type::get_connection_type();
}

void NetplaySession::record_peer_address() {
    // For log/UI only — we don't need the actual peer IP here because ENet
    // already has it.
    common::logger::info("session: peer connected ({})",
                         config_.is_host ? "client" : "host");
}

// ---- Step handlers ------------------------------------------------------

void NetplaySession::step_listening() {
    if (phase_timed_out()) {
        set_error("Connection timed out (no opponent connected in 1 hour)");
        state_ = SessionState::Failed;
        return;
    }
    // Smart host: drive relay client in parallel.
    if (relay_client_) {
        step_parallel_relay();
        if (state_ != SessionState::Listening) return;
    }
    common::net::TransportEvent ev;
    if (transport_.poll(0, ev) && ev == common::net::TransportEvent::Connected) {
        // Direct peer won — cancel relay.
        relay_client_.reset();
        relay_list_.clear();
        record_peer_address();
        state_ = SessionState::Handshaking;
        start_version_exchange();
    }
}

void NetplaySession::step_parallel_relay() {
    if (!relay_client_) return;

    if (relay_client_->state() == rclient::RelayState::Retrying) {
        set_status("Listening for direct connection... (retrying relay, attempt " +
                   std::to_string(relay_client_->retry_count()) + ")");
    }

    auto result = relay_client_->step();
    if (std::holds_alternative<rclient::InProgress>(result)) return;

    if (auto* r = std::get_if<rclient::RelayResult>(&result)) {
        // Relay succeeded — rebind transport to the hole-punch port.
        config_.local_udp_port = r->local_udp_port;
        config_.peer_port = r->local_udp_port;
        relay_client_.reset();
        relay_list_.clear();
        transport_.deinit();
        std::string err;
        if (!transport_.listen(r->local_udp_port, err)) {
            set_error(err);
            state_ = SessionState::Failed;
            return;
        }
        set_status("Connected via relay! Waiting for ENet connect...");
        // Stay in Listening — peer's ENet CONNECT will arrive at the new listener.
    } else if (auto* err_code = std::get_if<rclient::RelayError>(&result)) {
        // Relay failed — continue with direct-only.
        relay_client_.reset();
        relay_list_.clear();
        set_status("Listening for direct connection...");
        common::logger::warn("session: relay failed ({}), continuing direct-only",
                             rclient::error_label(*err_code));
    }
}

void NetplaySession::step_connecting() {
    if (phase_timed_out()) {
        set_error("Failed to connect to host (30s timeout)");
        state_ = SessionState::Failed;
        return;
    }
    common::net::TransportEvent ev;
    if (transport_.poll(0, ev)) {
        if (ev == common::net::TransportEvent::Connected) {
            record_peer_address();
            state_ = SessionState::Handshaking;
            start_version_exchange();
        } else if (ev == common::net::TransportEvent::Disconnected) {
            set_error("Host refused connection");
            state_ = SessionState::Failed;
        }
    }
}

void NetplaySession::step_relay() {
    if (!relay_client_) {
        set_error("Relay client not initialized");
        state_ = SessionState::Failed;
        return;
    }

    // Update status based on relay state.
    switch (relay_client_->state()) {
        case rclient::RelayState::TcpConnecting:
            set_status("Connecting to relay server..."); break;
        case rclient::RelayState::WaitingForHosted:
            set_status("Registering with relay..."); break;
        case rclient::RelayState::WaitingForMatchInfo:
            set_status(config_.is_host ? "Waiting for opponent to join..."
                                       : "Joining host via relay..."); break;
        case rclient::RelayState::WaitingForTunInfo:
            set_status("Negotiating connection..."); break;
        case rclient::RelayState::HolePunching:
            set_status("Hole-punching through NAT..."); break;
        case rclient::RelayState::Retrying:
            set_status("Retrying relay connection (attempt " +
                       std::to_string(relay_client_->retry_count()) + ")..."); break;
        default: break;
    }

    auto result = relay_client_->step();
    if (std::holds_alternative<rclient::InProgress>(result)) return;

    if (auto* r = std::get_if<rclient::RelayResult>(&result)) {
        // Success — hand off to ENet.
        config_.local_udp_port = r->local_udp_port;
        if (config_.is_host) {
            config_.peer_port = r->local_udp_port;
        } else {
            // Format peer IP as string.
            char ip[16];
            std::snprintf(ip, sizeof(ip), "%u.%u.%u.%u",
                          r->peer_ip[0], r->peer_ip[1], r->peer_ip[2], r->peer_ip[3]);
            config_.peer_addr = ip;
            config_.peer_port = r->peer_port;
        }
        relay_client_.reset();
        relay_list_.clear();

        std::string err;
        if (config_.is_host) {
            if (!transport_.listen(r->local_udp_port, err)) {
                set_error(err);
                state_ = SessionState::Failed;
                return;
            }
            state_ = SessionState::Listening;
            set_phase_timeout(kListenTimeoutMs);
            set_status("Connected via relay! Waiting for ENet connect...");
        } else {
            if (!transport_.connect_bound(config_.peer_addr, r->peer_port,
                                            r->local_udp_port, err)) {
                set_error(err);
                state_ = SessionState::Failed;
                return;
            }
            state_ = SessionState::Connecting;
            set_phase_timeout(kConnectTimeoutMs);
            set_status("Connecting to peer via relay...");
        }
    } else if (auto* err_code = std::get_if<rclient::RelayError>(&result)) {
        if (config_.is_host) {
            // Fallback to direct listen on the original port.
            relay_client_.reset();
            relay_list_.clear();
            std::string err;
            if (!transport_.listen(config_.peer_port, err)) {
                set_error(err);
                state_ = SessionState::Failed;
                return;
            }
            state_ = SessionState::Listening;
            set_phase_timeout(kListenTimeoutMs);
            set_status("Listening for direct connection... (relay failed)");
        } else {
            set_error(std::string(rclient::error_label(*err_code)) + ". " +
                      rclient::error_suggestion(*err_code));
            state_ = SessionState::Failed;
        }
    }
}

void NetplaySession::step_handshaking() {
    switch (handshake_subphase_) {
        case 0: step_exchange_version(); return;
        case 1: step_exchange_names();   return;
        case 3: step_exchange_config();  return;
        default: return;
    }
}

// ---- Handshake subphases ------------------------------------------------

void NetplaySession::start_version_exchange() {
    handshake_subphase_ = 0;
    char buf[130];
    buf[0] = static_cast<char>(MsgTag::Version);
    std::uint8_t ver_len = static_cast<std::uint8_t>(
        std::min<std::size_t>(std::strlen(kLocalVersion), 126));
    buf[1] = static_cast<char>(ver_len);
    std::memcpy(buf + 2, kLocalVersion, ver_len);
    transport_.send_reliable(buf, 2 + ver_len);
    set_phase_timeout(kVersionTimeoutMs);
}

void NetplaySession::step_exchange_version() {
    if (phase_timed_out()) {
        set_error("Version exchange timed out");
        state_ = SessionState::Failed;
        return;
    }
    common::net::TransportEvent ev;
    if (transport_.poll(0, ev)) {
        if (ev == common::net::TransportEvent::Disconnected) {
            set_error("Peer disconnected during handshake");
            state_ = SessionState::Failed;
            return;
        }
        if (ev == common::net::TransportEvent::MessageReceived) {
            auto msg = transport_.last_message();
            if (msg.size() >= 2 && static_cast<std::uint8_t>(msg[0]) ==
                static_cast<std::uint8_t>(MsgTag::Version)) {
                std::uint8_t ver_len = static_cast<std::uint8_t>(msg[1]);
                if (msg.size() < 2 + ver_len) {
                    set_error("Malformed version message");
                    state_ = SessionState::Failed;
                    return;
                }
                std::string peer_ver(msg.data() + 2, ver_len);
                if (peer_ver != kLocalVersion) {
                    set_error("Version mismatch: local=" + std::string(kLocalVersion) +
                              " remote=" + peer_ver);
                    state_ = SessionState::Failed;
                    return;
                }
                // Version matches — proceed to name exchange.
                start_name_exchange();
            }
        }
    }
}

void NetplaySession::start_name_exchange() {
    handshake_subphase_ = 1;
    char buf[128];
    std::size_t i = 0;
    buf[i++] = static_cast<char>(MsgTag::Name);
    std::uint8_t name_len = static_cast<std::uint8_t>(
        std::min<std::size_t>(config_.local_name.size(), 31));
    buf[i++] = static_cast<char>(name_len);
    std::memcpy(buf + i, config_.local_name.data(), name_len);
    i += name_len;
    std::uint8_t ct_len = static_cast<std::uint8_t>(
        std::min<std::size_t>(config_.local_connection_type.size(), 15));
    buf[i++] = static_cast<char>(ct_len);
    std::memcpy(buf + i, config_.local_connection_type.data(), ct_len);
    i += ct_len;
    transport_.send_reliable(buf, i);
    set_phase_timeout(kNameTimeoutMs);
}

void NetplaySession::step_exchange_names() {
    if (phase_timed_out()) {
        // Name exchange timeout is NON-fatal — continue with empty remote name.
        common::logger::warn("session: name exchange timed out (non-fatal)");
        start_ping_exchange();
        return;
    }
    common::net::TransportEvent ev;
    if (transport_.poll(0, ev)) {
        if (ev == common::net::TransportEvent::Disconnected) {
            set_error("Peer disconnected during handshake");
            state_ = SessionState::Failed;
            return;
        }
        if (ev == common::net::TransportEvent::MessageReceived) {
            auto msg = transport_.last_message();
            if (msg.size() >= 2 && static_cast<std::uint8_t>(msg[0]) ==
                static_cast<std::uint8_t>(MsgTag::Name)) {
                std::uint8_t name_len = static_cast<std::uint8_t>(msg[1]);
                if (msg.size() >= 2 + name_len + 1) {
                    config_.remote_name.assign(msg.data() + 2, name_len);
                    std::uint8_t ct_len = static_cast<std::uint8_t>(msg[2 + name_len]);
                    if (msg.size() >= 2 + name_len + 1 + ct_len) {
                        config_.remote_connection_type.assign(
                            msg.data() + 2 + name_len + 1, ct_len);
                    }
                }
                start_ping_exchange();
            }
        }
    }
}

void NetplaySession::start_ping_exchange() {
    state_ = SessionState::PingExchanging;
    ping_index_ = 0;
    set_phase_timeout(0);
    send_one_ping();
}

void NetplaySession::send_one_ping() {
    ping_start_ms_ = now_ms();
    char buf[9];
    buf[0] = static_cast<char>(MsgTag::Ping);
    std::uint64_t ts = static_cast<std::uint64_t>(ping_start_ms_);
    for (int i = 0; i < 8; ++i) {
        buf[1 + i] = static_cast<char>((ts >> (i * 8)) & 0xff);
    }
    transport_.send_reliable(buf, 9);
    // Per-ping timeout.
    set_phase_timeout(kPingPerAttemptTimeoutMs);
}

void NetplaySession::step_ping_exchanging() {
    if (phase_timed_out()) {
        // This ping timed out — move to the next one.
        ++ping_index_;
        if (ping_index_ >= 5) {
            finish_ping_exchange();
            return;
        }
        send_one_ping();
        return;
    }
    common::net::TransportEvent ev;
    if (transport_.poll(0, ev)) {
        if (ev == common::net::TransportEvent::Disconnected) {
            set_error("Peer disconnected during ping exchange");
            state_ = SessionState::Failed;
            return;
        }
        if (ev == common::net::TransportEvent::MessageReceived) {
            auto msg = transport_.last_message();
            if (msg.size() >= 1 && static_cast<std::uint8_t>(msg[0]) ==
                static_cast<std::uint8_t>(MsgTag::Ping)) {
                if (msg.size() >= 9) {
                    // Got a ping response — compute RTT.
                    std::int64_t now = now_ms();
                    std::int64_t rtt = now - ping_start_ms_;
                    stats_.count++;
                    stats_.avg_ms = (stats_.avg_ms * (stats_.count - 1) + rtt) /
                                     stats_.count;
                    if (stats_.min_ms == 0 || rtt < stats_.min_ms) {
                        stats_.min_ms = static_cast<double>(rtt);
                    }
                    if (rtt > stats_.max_ms) {
                        stats_.max_ms = static_cast<double>(rtt);
                    }
                    ++ping_index_;
                    if (ping_index_ >= 5) {
                        finish_ping_exchange();
                        return;
                    }
                    send_one_ping();
                } else {
                    // Stray ping (msg[0]==4 but <9 bytes) — echo it back.
                    transport_.send_reliable(msg.data(), msg.size());
                }
            }
        }
    }
}

void NetplaySession::finish_ping_exchange() {
    auto tstats = transport_.get_stats();
    stats_.packet_loss = static_cast<std::uint8_t>(tstats.packet_loss_pct);

    if (config_.is_host && !config_.manual_delay) {
        double avg = stats_.avg_ms > 0 ? stats_.avg_ms : 50.0;
        double computed = std::ceil(avg / (1000.0 / 60.0));
        config_.delay = static_cast<std::uint8_t>(
            std::min<double>(computed, 8.0));
    }

    if (config_.is_host) {
        config_.match_seed = static_cast<std::uint32_t>(
            static_cast<std::uint64_t>(now_ms()));
        state_ = SessionState::WaitingConfirmation;
        set_phase_timeout(0);
        notify_host_confirmation();
    } else {
        handshake_subphase_ = 3;
        state_ = SessionState::Handshaking;
        set_phase_timeout(0);
    }
}

void NetplaySession::send_config_message() {
    char buf[9];
    buf[0] = static_cast<char>(MsgTag::Config);
    buf[1] = static_cast<char>(config_.delay);
    buf[2] = static_cast<char>(config_.rollback);
    buf[3] = static_cast<char>(config_.win_count);
    buf[4] = static_cast<char>(config_.host_player);
    std::uint32_t seed = config_.match_seed;
    buf[5] = static_cast<char>(seed & 0xff);
    buf[6] = static_cast<char>((seed >> 8) & 0xff);
    buf[7] = static_cast<char>((seed >> 16) & 0xff);
    buf[8] = static_cast<char>((seed >> 24) & 0xff);
    transport_.send_reliable(buf, 9);
}

void NetplaySession::step_exchange_config() {
    if (config_.is_host) {
        if (phase_timed_out()) {
            set_error("Client never confirmed config (30s timeout)");
            state_ = SessionState::Failed;
            return;
        }
        common::net::TransportEvent ev;
        if (transport_.poll(0, ev)) {
            if (ev == common::net::TransportEvent::Disconnected) {
                set_error("Peer disconnected waiting for confirm");
                state_ = SessionState::Failed;
                return;
            }
            if (ev == common::net::TransportEvent::MessageReceived) {
                auto msg = transport_.last_message();
                if (msg.size() >= 1 && static_cast<std::uint8_t>(msg[0]) ==
                    static_cast<std::uint8_t>(MsgTag::Confirm)) {
                    state_ = SessionState::Launching;
                }
            }
        }
    } else {
        // Client: wait for config, then send confirm.
        common::net::TransportEvent ev;
        if (transport_.poll(0, ev)) {
            if (ev == common::net::TransportEvent::Disconnected) {
                set_error("Host disconnected during config exchange");
                state_ = SessionState::Failed;
                return;
            }
            if (ev == common::net::TransportEvent::MessageReceived) {
                auto msg = transport_.last_message();
                if (msg.size() >= 5 && static_cast<std::uint8_t>(msg[0]) ==
                    static_cast<std::uint8_t>(MsgTag::Config)) {
                    config_.delay = static_cast<std::uint8_t>(msg[1]);
                    config_.rollback = static_cast<std::uint8_t>(msg[2]);
                    config_.win_count = static_cast<std::uint8_t>(msg[3]);
                    config_.host_player = static_cast<std::uint8_t>(msg[4]);
                    if (msg.size() >= 9) {
                        config_.match_seed =
                            static_cast<std::uint32_t>(static_cast<std::uint8_t>(msg[5])) |
                            (static_cast<std::uint32_t>(static_cast<std::uint8_t>(msg[6])) << 8) |
                            (static_cast<std::uint32_t>(static_cast<std::uint8_t>(msg[7])) << 16) |
                            (static_cast<std::uint32_t>(static_cast<std::uint8_t>(msg[8])) << 24);
                    } else {
                        common::logger::warn("session: legacy v2 config (no match_seed)");
                        config_.match_seed = 0;
                    }
                    // Send confirm.
                    char confirm[1] = {static_cast<char>(MsgTag::Confirm)};
                    transport_.send_reliable(confirm, 1);
                    state_ = SessionState::Launching;
                }
            }
        }
    }
}

void NetplaySession::notify_host_confirmation() {
    auto hwnd = common::win32::window::find_launcher_hwnd();
    if (hwnd != common::win32::window::kInvalidHandle) {
        common::win32::window::flash(hwnd);
    }
    common::win32::window::beep();
}

} // namespace caster::exe::session
