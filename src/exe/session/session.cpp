// src/exe/session/session.cpp
//
// NetplaySession implementation — worker-thread edition.
//
// All ENet/relay operations run on the session's internal `std::jthread`
// (the "session worker"). The UI thread enqueues commands via `*_async()`
// methods and reads state via `snapshot()`.
//
// The handshake state machine (step_listening, step_handshaking, etc.) is
// unchanged from the single-threaded version — it still runs step-by-step.
// The only difference is that step() is now called in a loop by the worker
// instead of once per frame by the UI.

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
#include <utility>

namespace caster::exe::session {

namespace {

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

// Worker loop sleep between steps. ~120fps is responsive enough for
// handshake (which is wall-clock-timeout-driven, not frame-driven) and
// keeps CPU usage low.
constexpr auto kWorkerSleep = std::chrono::milliseconds(8);

// Handshake wire protocol message tags.
enum class MsgTag : std::uint8_t {
    Version = 1,
    Name    = 2,
    Ping    = 3,
    Config  = 4,
    Confirm = 5,
};

} // namespace

// ============================================================================
// Construction / destruction
// ============================================================================

NetplaySession::NetplaySession()
    : worker_([this](std::stop_token st) { worker_loop(st); }) {
    publish_snapshot();
}

NetplaySession::~NetplaySession() {
    // Enqueue Deinit so the worker cleans up the transport/relay before
    // the jthread stops. Then request_stop — the worker wakes from
    // wait_and_pop immediately thanks to stop_token support.
    commands_.push(session_command::Deinit{});
    worker_.request_stop();
    // jthread destructor joins.
}

// ============================================================================
// Async command API
// ============================================================================

void NetplaySession::start_host_async(std::uint16_t port, bool training) {
    commands_.push(session_command::StartHost{port, training});
}
void NetplaySession::start_smart_host_async(const std::string& relay_source,
                                            std::uint16_t port, bool training) {
    commands_.push(session_command::StartSmartHost{relay_source, port, training});
}
void NetplaySession::start_join_async(const std::string& host, std::uint16_t port, bool training) {
    commands_.push(session_command::StartJoin{host, port, training});
}
void NetplaySession::start_smart_join_async(const std::string& input,
                                            const std::string& relay_source, bool training) {
    commands_.push(session_command::StartSmartJoin{input, relay_source, training});
}
void NetplaySession::start_relay_host_async(const std::string& relay_source,
                                            std::uint16_t port, bool training) {
    commands_.push(session_command::StartRelayHost{relay_source, port, training});
}
void NetplaySession::start_relay_join_async(const std::string& relay_source,
                                            const std::string& peer_identifier, bool training) {
    commands_.push(session_command::StartRelayJoin{relay_source, peer_identifier, training});
}

void NetplaySession::set_local_name_async(const std::string& name) {
    commands_.push(session_command::SetLocalName{name});
}
void NetplaySession::set_manual_delay_async(std::uint8_t delay) {
    commands_.push(session_command::SetManualDelay{delay});
}
void NetplaySession::set_rollback_async(std::uint8_t rollback) {
    commands_.push(session_command::SetRollback{rollback});
}
void NetplaySession::lookup_host_addresses_async() {
    commands_.push(session_command::LookupHostAddresses{});
}
void NetplaySession::detect_connection_type_async() {
    commands_.push(session_command::DetectConnectionType{});
}

void NetplaySession::host_confirm_async() {
    commands_.push(session_command::HostConfirm{});
}
void NetplaySession::cancel_async() {
    commands_.push(session_command::Cancel{});
}

void NetplaySession::deinit_async() {
    commands_.push(session_command::Deinit{});
}

// ============================================================================
// Snapshot
// ============================================================================

SessionSnapshot NetplaySession::snapshot() const {
    std::lock_guard<std::mutex> lock(snapshot_mutex_);
    return snapshot_;
}

void NetplaySession::publish_snapshot() {
    std::lock_guard<std::mutex> lock(snapshot_mutex_);
    snapshot_.state             = state_;
    snapshot_.config            = config_;
    snapshot_.stats             = stats_;
    snapshot_.error_message     = error_message_;
    snapshot_.status_message    = status_message_;
    snapshot_.public_ip         = public_ip_;
    snapshot_.local_ip          = local_ip_;
    snapshot_.remaining_seconds = [&]() -> std::optional<std::uint32_t> {
        if (phase_deadline_ms_ == 0) return std::nullopt;
        std::int64_t now = now_ms();
        if (now >= phase_deadline_ms_) return 0;
        return static_cast<std::uint32_t>((phase_deadline_ms_ - now) / 1000);
    }();
    snapshot_.room_code         = [&]() -> std::optional<std::string> {
        if (!relay_client_) return std::nullopt;
        return relay_client_->get_room_code();
    }();
    snapshot_.room_validation   = room_validation_;
}

// ============================================================================
// Worker thread
// ============================================================================

void NetplaySession::worker_loop(std::stop_token st) {
    using namespace std::chrono_literals;

    // The worker loop must call step() CONTINUOUSLY — the handshake state
    // machine is driven by wall-clock timeouts and ENet polls that need to
    // run every few ms. We cannot block on wait_and_pop() between steps,
    // because that would freeze the handshake (no ENet polls → no connect,
    // no timeout checks → stale UI).
    //
    // Instead: drain_commands (non-blocking try_pop) → step() → publish →
    // short sleep (8ms ≈ 125Hz). The sleep keeps CPU usage low while
    // allowing the handshake to progress. Commands are picked up on the
    // next iteration (max 8ms latency, well below any handshake timeout).
    while (!st.stop_requested()) {
        drain_commands();
        step();
        publish_snapshot();
        std::this_thread::sleep_for(kWorkerSleep);
    }

    // Final drain on shutdown — process any pending Deinit.
    drain_commands();
    publish_snapshot();
}

void NetplaySession::drain_commands() {
    session_command::Command cmd;
    while (commands_.try_pop(cmd)) {
        apply_command(cmd);
    }
}

void NetplaySession::apply_command(const session_command::Command& cmd) {
    using namespace session_command;
    std::visit([this](const auto& c) {
        using T = std::decay_t<decltype(c)>;
        if constexpr (std::is_same_v<T, StartHost>) {
            // Reuses the old start_host logic (now inline).
            // (Moved here from the deleted start_host method.)
            if (state_ != SessionState::Idle) return;
            // Preserve user-set fields (local_name, local_connection_type,
            // manual_delay, delay, rollback) across the config reset.
            // These are set by SetLocalName/DetectConnectionType/SetManualDelay/
            // SetRollback commands which are enqueued BEFORE the Start* command.
            // Without this, config_ = {} wipes them.
            std::string saved_local_name = config_.local_name;
            std::string saved_local_conn_type = config_.local_connection_type;
            bool saved_manual_delay = config_.manual_delay;
            std::uint8_t saved_delay = config_.delay;
            std::uint8_t saved_rollback = config_.rollback;
            config_ = {};
            config_.local_name = saved_local_name;
            config_.local_connection_type = saved_local_conn_type;
            config_.manual_delay = saved_manual_delay;
            config_.delay = saved_delay;
            config_.rollback = saved_rollback;
            config_.is_host = true;
            config_.is_training = c.training;
            config_.is_netplay = true;
            config_.host_player = 1;
            config_.local_player = 1;
            config_.remote_player = 2;
            std::string err;
            if (!transport_.listen(c.port, err)) {
                set_error(err);
                state_ = SessionState::Failed;
                return;
            }
            config_.local_udp_port = c.port;
            config_.peer_port = c.port;
            state_ = SessionState::Listening;
            set_phase_timeout(kListenTimeoutMs);
            set_status("Listening for direct connection...");
        } else if constexpr (std::is_same_v<T, StartSmartHost>) {
            // Moved from start_smart_host.
            if (state_ != SessionState::Idle) return;
            // Preserve user-set fields (local_name, local_connection_type,
            // manual_delay, delay, rollback) across the config reset.
            // These are set by SetLocalName/DetectConnectionType/SetManualDelay/
            // SetRollback commands which are enqueued BEFORE the Start* command.
            // Without this, config_ = {} wipes them.
            std::string saved_local_name = config_.local_name;
            std::string saved_local_conn_type = config_.local_connection_type;
            bool saved_manual_delay = config_.manual_delay;
            std::uint8_t saved_delay = config_.delay;
            std::uint8_t saved_rollback = config_.rollback;
            config_ = {};
            config_.local_name = saved_local_name;
            config_.local_connection_type = saved_local_conn_type;
            config_.manual_delay = saved_manual_delay;
            config_.delay = saved_delay;
            config_.rollback = saved_rollback;
            config_.is_host = true;
            config_.is_training = c.training;
            config_.is_netplay = true;
            config_.host_player = 1;
            config_.local_player = 1;
            config_.remote_player = 2;
            std::string err;
            if (!transport_.listen(c.port, err)) {
                set_error(err);
                state_ = SessionState::Failed;
                return;
            }
            config_.local_udp_port = c.port;
            config_.peer_port = c.port;
            if (c.relay_source.empty()) {
                relay_list_ = rc::default_list();
            } else {
                relay_list_ = rc::parse_list(c.relay_source);
            }
            if (!relay_list_.empty()) {
                rclient::RelayClientInit init;
                init.relay = relay_list_[0];
                init.role = rclient::ClientRole::Host;
                init.local_port = c.port;
                init.external_udp_socket = transport_.udp_socket_fd();
                relay_client_ = std::make_unique<rclient::RelayClient>(init);
                transport_.set_relay_sink(relay_client_.get());
                transport_.install_intercept();
            }
            state_ = SessionState::Listening;
            set_phase_timeout(kListenTimeoutMs);
            set_status("Listening for direct connection...");
        } else if constexpr (std::is_same_v<T, StartJoin>) {
            // Moved from start_join.
            if (state_ != SessionState::Idle) return;
            // Preserve user-set fields (local_name, local_connection_type,
            // manual_delay, delay, rollback) across the config reset.
            // These are set by SetLocalName/DetectConnectionType/SetManualDelay/
            // SetRollback commands which are enqueued BEFORE the Start* command.
            // Without this, config_ = {} wipes them.
            std::string saved_local_name = config_.local_name;
            std::string saved_local_conn_type = config_.local_connection_type;
            bool saved_manual_delay = config_.manual_delay;
            std::uint8_t saved_delay = config_.delay;
            std::uint8_t saved_rollback = config_.rollback;
            config_ = {};
            config_.local_name = saved_local_name;
            config_.local_connection_type = saved_local_conn_type;
            config_.manual_delay = saved_manual_delay;
            config_.delay = saved_delay;
            config_.rollback = saved_rollback;
            config_.is_host = false;
            config_.is_training = c.training;
            config_.is_netplay = true;
            config_.host_player = 2;
            config_.local_player = 2;
            config_.remote_player = 1;
            config_.peer_addr = c.host;
            config_.peer_port = c.port;
            std::string err;
            if (!transport_.connect(c.host, c.port, err)) {
                set_error(err);
                state_ = SessionState::Failed;
                return;
            }
            state_ = SessionState::Connecting;
            set_phase_timeout(kConnectTimeoutMs);
            set_status("Connecting to " + c.host + ":" + std::to_string(c.port) + "...");
        } else if constexpr (std::is_same_v<T, StartSmartJoin>) {
            // Moved from start_smart_join.
            auto parsed = cd::parse_input(c.input);
            if (parsed.type == cd::InputType::RoomCode) {
                apply_command(StartRelayJoin{c.relay_source, parsed.room_code, c.training});
            } else if (parsed.type == cd::InputType::IpPort) {
                apply_command(StartJoin{parsed.host, static_cast<std::uint16_t>(parsed.port), c.training});
            } else {
                set_error("Invalid input. Use host:port or #room");
                state_ = SessionState::Failed;
            }
        } else if constexpr (std::is_same_v<T, StartRelayHost>) {
            // Moved from start_relay_host.
            if (state_ != SessionState::Idle) return;
            // Preserve user-set fields (local_name, local_connection_type,
            // manual_delay, delay, rollback) across the config reset.
            // These are set by SetLocalName/DetectConnectionType/SetManualDelay/
            // SetRollback commands which are enqueued BEFORE the Start* command.
            // Without this, config_ = {} wipes them.
            std::string saved_local_name = config_.local_name;
            std::string saved_local_conn_type = config_.local_connection_type;
            bool saved_manual_delay = config_.manual_delay;
            std::uint8_t saved_delay = config_.delay;
            std::uint8_t saved_rollback = config_.rollback;
            config_ = {};
            config_.local_name = saved_local_name;
            config_.local_connection_type = saved_local_conn_type;
            config_.manual_delay = saved_manual_delay;
            config_.delay = saved_delay;
            config_.rollback = saved_rollback;
            if (c.relay_source.empty()) {
                relay_list_ = rc::default_list();
            } else {
                relay_list_ = rc::parse_list(c.relay_source);
            }
            if (relay_list_.empty()) {
                set_error("No relay servers configured");
                state_ = SessionState::Failed;
                return;
            }
            std::string err;
            if (!transport_.listen(c.port, err)) {
                set_error(err);
                state_ = SessionState::Failed;
                return;
            }
            rclient::RelayClientInit init;
            init.relay = relay_list_[0];
            init.role = rclient::ClientRole::Host;
            init.local_port = c.port;
            init.external_udp_socket = transport_.udp_socket_fd();
            relay_client_ = std::make_unique<rclient::RelayClient>(init);
            transport_.set_relay_sink(relay_client_.get());
            transport_.install_intercept();
            config_.is_host = true;
            config_.is_training = c.training;
            config_.is_netplay = true;
            config_.host_player = 1;
            config_.local_player = 1;
            config_.remote_player = 2;
            config_.local_udp_port = c.port;
            config_.peer_port = c.port;
            state_ = SessionState::RelayConnecting;
            set_phase_timeout(0);
            set_status("Connecting to relay server...");
        } else if constexpr (std::is_same_v<T, StartRelayJoin>) {
            // Moved from start_relay_join.
            if (state_ != SessionState::Idle) return;
            // Preserve user-set fields (local_name, local_connection_type,
            // manual_delay, delay, rollback) across the config reset.
            // These are set by SetLocalName/DetectConnectionType/SetManualDelay/
            // SetRollback commands which are enqueued BEFORE the Start* command.
            // Without this, config_ = {} wipes them.
            std::string saved_local_name = config_.local_name;
            std::string saved_local_conn_type = config_.local_connection_type;
            bool saved_manual_delay = config_.manual_delay;
            std::uint8_t saved_delay = config_.delay;
            std::uint8_t saved_rollback = config_.rollback;
            config_ = {};
            config_.local_name = saved_local_name;
            config_.local_connection_type = saved_local_conn_type;
            config_.manual_delay = saved_manual_delay;
            config_.delay = saved_delay;
            config_.rollback = saved_rollback;
            if (c.relay_source.empty()) {
                relay_list_ = rc::default_list();
            } else {
                relay_list_ = rc::parse_list(c.relay_source);
            }
            if (relay_list_.empty()) {
                set_error("No relay servers configured");
                state_ = SessionState::Failed;
                return;
            }
            std::string err;
            if (!transport_.bind_only(0, err)) {
                set_error(err);
                state_ = SessionState::Failed;
                return;
            }
            rclient::RelayClientInit init;
            init.relay = relay_list_[0];
            init.role = rclient::ClientRole::Client;
            init.local_port = 0;
            init.peer_identifier = c.peer_identifier;
            init.external_udp_socket = transport_.udp_socket_fd();
            relay_client_ = std::make_unique<rclient::RelayClient>(init);
            transport_.set_relay_sink(relay_client_.get());
            transport_.install_intercept();
            config_.is_host = false;
            config_.is_training = c.training;
            config_.is_netplay = true;
            config_.host_player = 2;
            config_.local_player = 2;
            config_.remote_player = 1;
            state_ = SessionState::RelayConnecting;
            set_phase_timeout(0);
            set_status("Connecting to relay server...");
        } else if constexpr (std::is_same_v<T, SetLocalName>) {
            config_.local_name = c.name;
        } else if constexpr (std::is_same_v<T, SetManualDelay>) {
            config_.manual_delay = true;
            config_.delay = c.delay;
        } else if constexpr (std::is_same_v<T, SetRollback>) {
            config_.rollback = c.rollback;
        } else if constexpr (std::is_same_v<T, LookupHostAddresses>) {
            auto pub = common::net::ip_discovery::get_public_ip();
            if (!pub.empty()) public_ip_ = pub;
            auto loc = common::net::ip_discovery::get_local_ip();
            if (!loc.empty()) local_ip_ = loc;
        } else if constexpr (std::is_same_v<T, DetectConnectionType>) {
            config_.local_connection_type = common::net::connection_type::get_connection_type();
        } else if constexpr (std::is_same_v<T, HostConfirm>) {
            if (state_ != SessionState::WaitingConfirmation) return;
            host_confirmed_ = true;
            send_config_message();
            handshake_subphase_ = 3;
            set_phase_timeout(kHostWaitConfirmTimeoutMs);
            state_ = SessionState::Handshaking;
        } else if constexpr (std::is_same_v<T, Cancel>) {
            cancel_requested_ = true;
        } else if constexpr (std::is_same_v<T, Deinit>) {
            transport_.deinit();
            relay_client_.reset();
            relay_list_.clear();
            state_ = SessionState::Idle;
            error_message_.clear();
            status_message_.clear();
            cancel_requested_ = false;
            host_confirmed_ = false;
            handshake_subphase_ = 0;
            ping_index_ = 0;
            phase_start_ms_ = 0;
            phase_deadline_ms_ = 0;
        }
    }, cmd);
}

// ============================================================================
// Helpers
// ============================================================================

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

void NetplaySession::maybe_heartbeat() {
    if (!transport_.is_connected()) return;
    std::int64_t now = now_ms();
    if (now - last_heartbeat_ms_ >= kHeartbeatIntervalMs) {
        transport_.ping();
        last_heartbeat_ms_ = now;
    }
}

void NetplaySession::record_peer_address() {
    common::logger::info("session: peer connected ({})",
                         config_.is_host ? "client" : "host");
}

// ============================================================================
// State machine (unchanged logic, now runs on the worker thread)
// ============================================================================

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

void NetplaySession::step_listening() {
    if (phase_timed_out()) {
        set_error("Connection timed out (no opponent connected in 1 hour)");
        state_ = SessionState::Failed;
        return;
    }
    if (relay_client_) {
        step_parallel_relay();
        if (state_ != SessionState::Listening) return;
    }
    common::net::TransportEvent ev;
    if (transport_.poll(0, ev) && ev == common::net::TransportEvent::Connected) {
        transport_.set_relay_sink(nullptr);
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
        config_.local_udp_port = r->local_udp_port;
        config_.peer_port = r->local_udp_port;
        transport_.set_relay_sink(nullptr);
        relay_client_.reset();
        relay_list_.clear();
        set_status("Connected via relay! Waiting for ENet connect...");
    } else if (auto* err_code = std::get_if<rclient::RelayError>(&result)) {
        transport_.set_relay_sink(nullptr);
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

    switch (relay_client_->state()) {
        case rclient::RelayState::TcpConnecting:
            set_status("Connecting to relay server (zzcaster.duckdns.org:3939)..."); break;
        case rclient::RelayState::WaitingForHosted:
            set_status("Registering room with relay..."); break;
        case rclient::RelayState::WaitingForMatchInfo:
            set_status(config_.is_host
                ? "Waiting for opponent to join your room..."
                : "Joining host's room via relay..."); break;
        case rclient::RelayState::WaitingForTunInfo:
            set_status("Negotiating connection details with relay..."); break;
        case rclient::RelayState::HolePunching:
            set_status("Hole-punching through NAT (this can take a few seconds)..."); break;
        case rclient::RelayState::Retrying:
            set_status("Retrying relay connection (attempt " +
                       std::to_string(relay_client_->retry_count()) + ")..."); break;
        default: break;
    }

    auto result = relay_client_->step();
    if (std::holds_alternative<rclient::InProgress>(result)) {
        if (transport_.is_shared_socket()) {
            common::net::TransportEvent ev;
            transport_.poll(0, ev);
        }
        return;
    }

    if (auto* r = std::get_if<rclient::RelayResult>(&result)) {
        config_.local_udp_port = r->local_udp_port;
        if (config_.is_host) {
            config_.peer_port = r->local_udp_port;
            state_ = SessionState::Listening;
            set_phase_timeout(kListenTimeoutMs);
            set_status("Connected via relay! Waiting for ENet connect...");
        } else {
            char ip[16];
            std::snprintf(ip, sizeof(ip), "%u.%u.%u.%u",
                          r->peer_ip[0], r->peer_ip[1], r->peer_ip[2], r->peer_ip[3]);
            config_.peer_addr = ip;
            config_.peer_port = r->peer_port;

            std::string err;
            if (!transport_.connect_to_peer(config_.peer_addr, r->peer_port, err)) {
                set_error(err);
                state_ = SessionState::Failed;
                return;
            }
            state_ = SessionState::Connecting;
            set_phase_timeout(kConnectTimeoutMs);
            set_status("Connecting to peer via relay...");
        }
        transport_.set_relay_sink(nullptr);
        relay_client_.reset();
        relay_list_.clear();
    } else if (auto* err_code = std::get_if<rclient::RelayError>(&result)) {
        transport_.set_relay_sink(nullptr);
        relay_client_.reset();
        relay_list_.clear();
        set_error(std::string(rclient::error_label(*err_code)) + ". " +
                  rclient::error_suggestion(*err_code));
        state_ = SessionState::Failed;
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
    set_phase_timeout(kPingPerAttemptTimeoutMs);
}

void NetplaySession::step_ping_exchanging() {
    if (phase_timed_out()) {
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
                    transport_.send_reliable(msg.data(), msg.size());
                }
            }
        }
    }
}

void NetplaySession::finish_ping_exchange() {
    auto tstats = transport_.get_stats();
    stats_.packet_loss = static_cast<std::uint8_t>(tstats.packet_loss_pct);

    // ---- Suggested delay / rollback calculation ----------------------
    // Based on RTT (avg ping). Always prefers higher rollback over delay
    // so the game feels responsive — delay only increases when the
    // connection is so bad that rollback alone can't cover the one-way trip.
    //
    //   rtt < 50ms   → delay=0, rollback=4
    //   rtt < 100ms  → delay=1, rollback=6
    //   rtt < 150ms  → delay=2, rollback=7   (intermediate tier)
    //   otherwise    → delay=3, rollback=8
    //   if rtt > 180ms (very bad), stretch delay using one-way + jitter margin
    if (config_.is_host && !config_.manual_delay) {
        // If avg_ms is 0 (all pings timed out), assume a bad connection
        // rather than a perfect one — use 200ms so the tiers select a
        // conservative delay/rollback.
        const double avg = stats_.avg_ms > 0 ? stats_.avg_ms : 200.0;
        const double frame_ms = 1000.0 / 60.0;  // ~16.67ms

        int delay = 0;
        int rollback = 4;

        if (avg < 50.0) {
            delay = 0;
            rollback = 4;
        } else if (avg < 100.0) {
            delay = 1;
            rollback = 6;
        } else if (avg < 150.0) {
            delay = 2;
            rollback = 7;
        } else {
            delay = 3;
            rollback = 8;
        }

        // Very bad connection (>180ms): stretch delay so the total
        // (delay + rollback) covers the one-way trip + a 2-frame jitter
        // margin. Using one-way (RTT/2) instead of full RTT avoids
        // inflating the delay ~2× more than necessary.
        if (avg > 180.0) {
            const int one_way_frames = static_cast<int>(
                std::ceil((avg / 2.0) / frame_ms));
            const int jitter_margin = 2;
            const int needed = one_way_frames + jitter_margin;
            // Only stretch if the tier's default (delay + rollback) is
            // insufficient. Without this check, the condition `needed >
            // rollback` would fire even when the tier total already covers
            // the RTT, silently REDUCING the delay (e.g. at 220ms RTT the
            // tier gives delay=3+rollback=8=11, but needed=9 would reduce
            // delay to 1).
            if (needed > delay + rollback) {
                delay = needed - rollback;
            }
        }

        config_.delay    = static_cast<std::uint8_t>(delay);
        config_.rollback = static_cast<std::uint8_t>(rollback);
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
