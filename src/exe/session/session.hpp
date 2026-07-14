// src/exe/session/session.hpp
//
// NetplaySession — state machine for the launcher-side handshake.
// Ported from zzcaster's `src/launcher/session.zig`, then refactored
// to run on a dedicated worker thread (Layer 1 of the threading
// migration — see docs/threading-migration.md).
//
// Threading model:
//   - All ENet/relay operations happen on the session's internal
//     `std::jthread` (the "session worker"). The UI thread never
//     touches `transport_` or `relay_client_` directly.
//   - The UI thread enqueues commands via `*_async()` methods and
//     reads state via `snapshot()` (a cheap copy under a mutex).
//   - The worker drains commands, runs `step()` (the handshake state
//     machine), and updates the snapshot under the mutex.
//
// This is a breaking change from the original single-threaded API
// (`step()`, `state()`, `config()`, etc. are gone). All callers were
// updated to use `snapshot()` + `*_async()`.

#pragma once

#include "netplay_config.hpp"
#include "../../common/concurrency.hpp"
#include "../../common/net/enet_transport.hpp"
#include "../../common/net/relay/relay_client.hpp"
#include "../../common/net/relay/relay_config.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <variant>

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

// Immutable snapshot of the session state, read by the UI thread.
// All fields are copies — safe to use without locking.
struct SessionSnapshot {
    SessionState    state               = SessionState::Idle;
    NetplayConfig   config;
    PingStats       stats;
    std::string     error_message;
    std::string     status_message;
    std::optional<std::string> public_ip;
    std::optional<std::string> local_ip;
    std::optional<std::uint32_t> remaining_seconds;
    std::optional<std::string> room_code;
    std::optional<common::net::relay_client::RoomValidationResult> room_validation;
};

// Commands enqueued by the UI thread, drained by the session worker.
// Keep this an internal type — callers use the typed `*_async()` methods.
namespace session_command {

struct StartHost {
    std::uint16_t port;
    bool training;
};
struct StartSmartHost {
    std::string relay_source;
    std::uint16_t port;
    bool training;
};
struct StartJoin {
    std::string host;
    std::uint16_t port;
    bool training;
};
// Phase C / Fase 4: spectator join. Same as StartJoin but with
// is_spectator = true on the resulting NetplayConfig.
struct StartSpectate {
    std::string host;
    std::uint16_t port;
};
// Phase C / Fase 5: spectator via relay. Same as StartRelayJoin but with
// is_spectator = true.
struct StartRelaySpectate {
    std::string relay_source;
    std::string room_code;
};
struct StartSmartJoin {
    std::string input;
    std::string relay_source;
    bool training;
};
struct StartRelayHost {
    std::string relay_source;
    std::uint16_t port;
    bool training;
};
struct StartRelayJoin {
    std::string relay_source;
    std::string peer_identifier;
    bool training;
};
struct SetLocalName      { std::string name; };
struct SetManualDelay    { std::uint8_t delay; };
struct SetRollback       { std::uint8_t rollback; };
struct LookupHostAddresses {};
struct DetectConnectionType {};
struct HostConfirm {};
struct Cancel {};
struct Deinit {};

using Command = std::variant<
    StartHost,
    StartSmartHost,
    StartJoin,
    StartSpectate,
    StartSmartJoin,
    StartRelayHost,
    StartRelayJoin,
    StartRelaySpectate,
    SetLocalName,
    SetManualDelay,
    SetRollback,
    LookupHostAddresses,
    DetectConnectionType,
    HostConfirm,
    Cancel,
    Deinit
>;

} // namespace session_command

class NetplaySession {
public:
    NetplaySession();
    ~NetplaySession();

    NetplaySession(const NetplaySession&)            = delete;
    NetplaySession& operator=(const NetplaySession&) = delete;
    NetplaySession(NetplaySession&&)                 = delete;
    NetplaySession& operator=(NetplaySession&&)      = delete;

    // ---- Async command API (enqueue + return immediately) ----
    //
    // Each method enqueues a command on the session worker. The worker
    // processes them in order. The UI reads the result via `snapshot()`.

    void start_host_async(std::uint16_t port, bool training);
    void start_smart_host_async(const std::string& relay_source,
                                std::uint16_t port, bool training);
    void start_join_async(const std::string& host, std::uint16_t port, bool training);
    // Phase C / Fase 4: spectator join. Same as start_join_async but sets
    // is_spectator = true on the NetplayConfig, so the DLL enters
    // SpectateNetplay mode and the SpectateClient is created.
    void start_spectate_async(const std::string& host, std::uint16_t port);
    // Phase C / Fase 5: spectator via relay. Same as start_relay_join_async
    // but sets is_spectator = true. The relay treats the spectator as a
    // normal client (same room code as the host); the host identifies the
    // spectator because it connects after the opponent is already paired.
    void start_relay_spectate_async(const std::string& relay_source,
                                     const std::string& room_code);
    void start_smart_join_async(const std::string& input,
                                const std::string& relay_source, bool training);
    void start_relay_host_async(const std::string& relay_source,
                                std::uint16_t port, bool training);
    void start_relay_join_async(const std::string& relay_source,
                                const std::string& peer_identifier, bool training);

    void set_local_name_async(const std::string& name);
    void set_manual_delay_async(std::uint8_t delay);
    void set_rollback_async(std::uint8_t rollback);
    void lookup_host_addresses_async();
    void detect_connection_type_async();

    void host_confirm_async();
    void cancel_async();

    // Tear down everything (relay + transport). Enqueues a Deinit command
    // and waits for the worker to process it. After this returns, the
    // session is back to Idle and can be restarted.
    void deinit_async();

    // ---- State access (thread-safe) ----
    //
    // Returns a consistent snapshot of the session state. Cheap (one mutex
    // lock + copy). Safe to call from the UI thread.

    SessionSnapshot snapshot() const;

private:
    // ---- Worker thread loop ----
    void worker_loop(std::stop_token st);

    // Drain pending commands (called from the worker thread).
    void drain_commands();

    // Apply a single command (called from the worker thread).
    void apply_command(const session_command::Command& cmd);

    // Update the snapshot under the mutex (called from the worker thread
    // after each step()).
    void publish_snapshot();

    // ---- Internal helpers (run on the worker thread) ----
    void set_phase_timeout(std::int64_t duration_ms);
    bool phase_timed_out() const;
    void set_error(const std::string& msg);
    void set_status(const std::string& msg);

    void maybe_heartbeat();

    void step();
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

    // ---- State (only touched from the worker thread) ----
    SessionState    state_              = SessionState::Idle;
    NetplayConfig   config_;
    PingStats       stats_;

    std::uint8_t    handshake_subphase_ = 0;
    std::uint32_t   ping_index_         = 0;
    std::int64_t    ping_start_ms_      = 0;
    std::int64_t    last_heartbeat_ms_  = 0;
    std::int64_t    phase_start_ms_     = 0;
    std::int64_t    phase_deadline_ms_  = 0;

    bool            cancel_requested_   = false;
    bool            host_confirmed_     = false;

    std::string     error_message_;
    std::string     status_message_;
    std::optional<std::string> public_ip_;
    std::optional<std::string> local_ip_;

    common::net::EnetTransport transport_;
    std::unique_ptr<common::net::relay_client::RelayClient> relay_client_;
    common::net::relay_config::RelayList relay_list_;
    std::optional<common::net::relay_client::RoomValidationResult> room_validation_;

    // ---- Threading machinery ----
    common::concurrency::BlockingQueue<session_command::Command> commands_;
    std::jthread   worker_;

    // ---- Snapshot (read by UI, written by worker) ----
    mutable std::mutex   snapshot_mutex_;
    SessionSnapshot      snapshot_;

    // Protocol version (compared exact-match with peer).
    static constexpr const char* kLocalVersion = "4.1-cpp";
};

} // namespace caster::exe::session
