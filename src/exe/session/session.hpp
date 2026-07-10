// src/exe/session/session.hpp
//
// NetplaySession — state machine for the launcher-side handshake.
// Ported from zzcaster's `src/launcher/session.zig`.
//
// Single-thread, frame-driven via step(). Wall-clock timeouts.
// The UI calls step() each frame and reads state via getters.

#pragma once

#include "netplay_config.hpp"
#include "../../common/net/enet_transport.hpp"
#include "../../common/net/relay/relay_client.hpp"
#include "../../common/net/relay/relay_config.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace caster::exe::session {

enum class SessionState {
    Idle,
    Listening,            // host: ENet listen active (direct / smart-host / post-relay)
    Connecting,           // client: ENet connect in progress
    Handshaking,          // subphases via handshake_subphase
    PingExchanging,       // 5 pings, ~833ms each
    WaitingConfirmation,  // host: post-ping, waiting for user click "Start Match"
    Launching,            // handshake complete, ready to launch game
    Completed,            // not actively used
    Failed,               // terminal error
    Cancelled,            // user cancelled
    RelayConnecting,      // relay handshake in progress
};

class NetplaySession {
public:
    NetplaySession();
    ~NetplaySession();

    NetplaySession(const NetplaySession&)            = delete;
    NetplaySession& operator=(const NetplaySession&) = delete;
    NetplaySession(NetplaySession&&)                 = delete;
    NetplaySession& operator=(NetplaySession&&)      = delete;

    // ---- Start methods (each transitions to an appropriate state) ----

    // Direct host: listen on `port`.
    bool start_host(std::uint16_t port, bool training);

    // Direct join: connect to host:port.
    bool start_join(const std::string& host_str, std::uint16_t port, bool training);

    // Smart host: direct listener + relay client in parallel (first peer wins).
    bool start_smart_host(const std::string& relay_source,
                          std::uint16_t port, bool training);

    // Smart join: parse input, decide between relay (room code) and direct (ip:port).
    bool start_smart_join(const std::string& input,
                          const std::string& relay_source, bool training);

    // Relay-only host: generates a room code.
    bool start_relay_host(const std::string& relay_source,
                          std::uint16_t port, bool training);

    // Relay-only join: use a room code.
    bool start_relay_join(const std::string& relay_source,
                          const std::string& peer_identifier, bool training);

    // ---- Drive the state machine ----
    void step();

    // Host: confirm the match (after user clicks "Start Match").
    void host_confirm();

    // Cancel — next step() will transition to Cancelled.
    void cancel() { cancel_requested_ = true; }

    // Tear down everything (relay + transport).
    void deinit();

    // ---- Getters for UI display ----
    SessionState state() const { return state_; }

    std::optional<std::string> public_ip() const { return public_ip_; }
    std::optional<std::string> local_ip() const  { return local_ip_; }
    std::string local_connection_type() const  { return config_.local_connection_type; }
    std::string remote_connection_type() const { return config_.remote_connection_type; }
    std::string local_name() const  { return config_.local_name; }
    std::string remote_name() const { return config_.remote_name; }
    std::string error_message() const { return error_message_; }
    std::string status_message() const;
    std::optional<std::uint32_t> remaining_seconds() const;
    std::optional<std::string> room_code() const;
    const PingStats& stats() const { return stats_; }
    const NetplayConfig& config() const { return config_; }

    // Room code validation result (set during start_relay_join before the
    // full handshake begins). The GUI reads this to show immediate feedback
    // when a room code is invalid / not found / relay unreachable.
    // Returns nullopt if no validation has been performed (e.g. host mode,
    // direct join, or validation succeeded and we proceeded to handshake).
    std::optional<common::net::relay_client::RoomValidationResult>
    room_validation() const { return room_validation_; }

    // Populate local_name from cfg.display_name.
    void set_local_name(const std::string& name) { config_.local_name = name; }

    // Override the auto-computed input delay (from RTT) with a manual
    // value. Sets manual_delay=true so the host's finish_ping_exchange
    // skips the auto-compute branch. Used by the --delay=N CLI flag.
    // Must be called BEFORE start_host/start_smart_host/start_join so
    // the value is in place when the handshake exchanges the config.
    void set_manual_delay(std::uint8_t delay) {
        config_.manual_delay = true;
        config_.delay = delay;
    }

    // Override the rollback window. Used by the --rollback=N CLI flag.
    // Must be called BEFORE start_host/start_smart_host/start_join so
    // the value is in place when the handshake exchanges the config.
    void set_rollback(std::uint8_t rollback) {
        config_.rollback = rollback;
    }

    // Discover public + local IP and connection type.
    void lookup_host_addresses();
    void detect_connection_type();

private:
    // ---- Internal helpers ----
    void set_phase_timeout(std::int64_t duration_ms);
    bool phase_timed_out() const;
    void set_error(const std::string& msg);
    void set_status(const std::string& msg);

    void maybe_heartbeat();

    void step_listening();
    void step_connecting();
    void step_handshaking();
    void step_ping_exchanging();
    void step_relay();
    void step_parallel_relay();  // smart host

    void start_version_exchange();
    void step_exchange_version();
    void start_name_exchange();
    void step_exchange_names();
    void start_ping_exchange();
    void send_one_ping();
    void finish_ping_exchange();
    void send_config_message();
    void step_exchange_config();
    void notify_host_confirmation();

    void record_peer_address();

    // ---- State ----
    SessionState    state_              = SessionState::Idle;
    NetplayConfig   config_;
    PingStats       stats_;

    // Handshake subphase (0=version, 1=names, 3=config).
    std::uint8_t    handshake_subphase_ = 0;

    // Ping state.
    std::uint32_t   ping_index_         = 0;
    std::int64_t    ping_start_ms_      = 0;

    // Heartbeat.
    std::int64_t    last_heartbeat_ms_  = 0;

    // Phase timeout anchors.
    std::int64_t    phase_start_ms_     = 0;
    std::int64_t    phase_deadline_ms_  = 0;

    // Flags.
    bool            cancel_requested_   = false;
    bool            host_confirmed_     = false;

    // Error/status strings.
    std::string     error_message_;
    std::string     status_message_;
    std::optional<std::string> public_ip_;
    std::optional<std::string> local_ip_;

    // Owned sub-systems.
    common::net::EnetTransport transport_;
    std::unique_ptr<common::net::relay_client::RelayClient> relay_client_;
    common::net::relay_config::RelayList relay_list_;

    // Room code validation result (populated by start_relay_join before the
    // full handshake; cleared on success).
    std::optional<common::net::relay_client::RoomValidationResult> room_validation_;

    // Protocol version (compared exact-match with peer).
    static constexpr const char* kLocalVersion = "4.1-cpp";
};

} // namespace caster::exe::session
