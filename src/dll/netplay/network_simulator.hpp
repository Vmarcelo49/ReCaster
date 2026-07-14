// src/dll/netplay/network_simulator.hpp
//
// Layer 4 — Network thread foundation (subtask 4.2).
//
// Standalone network simulator for testing rollback under laggy conditions.
// Extracted from connector.cpp (where it was inline) so that the
// NetworkThread can own it as a member without polluting its loop with
// simulator logic.
//
// Design decisions (see docs/threading-migration.md Layer 4, Decision 3):
//   - Built now (not deferred) because the receive path is being
//     rewritten for the network thread anyway — marginal cost of
//     encapsulating while already touching this code is lower than
//     doing it as a separate migration later.
//   - Phase B (speculative rollback) is the highest-risk milestone;
//     keeping the simulator available is essential for reproducing
//     desync scenarios locally.
//
// Threading model:
//   - The NetworkSimulator is owned by NetworkThread and accessed
//     ONLY by the network thread. No lock needed.
//   - configure() is called once from NetworkThread::start() (game
//     thread) BEFORE the jthread is spawned. After that, the simulator
//     is read-only from the network thread's perspective.
//   - This is safe because start() spawns the jthread AFTER configure()
//     returns, establishing a happens-before relationship.
//
// Behavior (matches the existing connector.cpp simulator):
//   - Applies only to PlayerInputs (UNRELIABLE, per-frame)
//   - Control messages (TransitionIndex, MenuIndex, RngState, SyncHash)
//     are never delayed — they use RELIABLE channels and delaying them
//     would break handshake/state-sync logic, not test rollback.
//   - Three knobs (read from env vars in configure()):
//       CASTER_SIM_LAG_MS=N     — base delay added to each PlayerInputs
//       CASTER_SIM_JITTER_MS=N  — random ±N ms added to the delay
//       CASTER_SIM_LOSS_PCT=N   — drop N% of PlayerInputs (0-100)
//       CASTER_SIM_SEED=N       — RNG seed for reproducible runs
//                                 (default: std::random_device)
//   - If none of LAG/JITTER/LOSS are set, the simulator is disabled
//     and all maybeDelay()/shouldDrop() calls are no-ops.

#pragma once

#include "protocol/messages.hpp"

#include <chrono>
#include <deque>
#include <optional>
#include <random>

namespace caster::dll::netplay {

class NetworkSimulator {
public:
    // Configuration. Set once via configure() before the network thread
    // starts polling. After that, treated as read-only by the network
    // thread.
    struct Config {
        bool enabled    = false;
        int  lag_ms     = 0;    // base delay added to each PlayerInputs
        int  jitter_ms  = 0;    // random ±jitter added to the delay
        int  loss_pct   = 0;    // 0-100, percent of PlayerInputs to drop
    };

    NetworkSimulator() = default;
    ~NetworkSimulator() = default;

    NetworkSimulator(const NetworkSimulator&)            = delete;
    NetworkSimulator& operator=(const NetworkSimulator&) = delete;
    NetworkSimulator(NetworkSimulator&&)                 = delete;
    NetworkSimulator& operator=(NetworkSimulator&&)      = delete;

    // Read CASTER_SIM_LAG_MS / CASTER_SIM_JITTER_MS /
    // CASTER_SIM_LOSS_PCT / CASTER_SIM_SEED from the environment and
    // populate config_ + seed the RNG.
    //
    // MUST be called from NetworkThread::start() (game thread) BEFORE
    // the jthread is spawned. After this returns, the simulator is
    // treated as read-only by the network thread.
    void configure();

    // True if any of lag_ms/jitter_ms/loss_pct is non-zero.
    bool enabled() const { return config_.enabled; }

    // ---- Receive path (called by NetworkThread on each PlayerInputs) ----

    // Returns true if this packet should be dropped (simulated packet loss).
    // Only meaningful when enabled() && loss_pct > 0.
    //
    // Called BEFORE maybeDelay(). If true, the NetworkThread discards
    // the packet entirely (no push to inbox, no delay queue).
    bool shouldDrop();

    // Returns nullopt for immediate delivery, or the delivery timestamp
    // for delayed delivery. When delayed, the caller should push to
    // delayQueue_ via enqueueDelayed() instead of to the inbox.
    //
    // Only meaningful when enabled() && (lag_ms > 0 || jitter_ms > 0).
    // Otherwise returns nullopt (deliver now).
    std::optional<std::chrono::steady_clock::time_point>
    maybeDelay(const PlayerInputs& pi);

    // Push a PlayerInputs onto the delay queue with the given delivery
    // timestamp (returned by maybeDelay()).
    void enqueueDelayed(PlayerInputs pi,
                        std::chrono::steady_clock::time_point deliver_at);

    // Push any delayed messages whose delivery time has elapsed into the
    // provided outbox. Called at the top of each NetworkThread loop
    // iteration, before enet_host_service().
    //
    // The outbox is the NetworkThread's inboxPlayerInputs_ queue.
    void deliverExpired(std::deque<PlayerInputs>& outbox);

    // Clear the delay queue (called on shutdown).
    void clear();

private:
    Config config_;

    // RNG for jitter + packet loss. Seeded once in configure() via
    // std::random_device, or via CASTER_SIM_SEED env var for
    // reproducible test runs.
    //
    // Replaces std::rand() from the original connector.cpp, which is
    // not thread-safe and would be a data race once the network thread
    // is the one calling it.
    std::mt19937 rng_;

    // Pending delayed messages, ordered by deliver_at. We use std::deque
    // (not std::priority_queue) because the original connector.cpp used
    // std::deque and we want to preserve FIFO behavior for messages
    // with the same timestamp.
    struct DelayedMessage {
        PlayerInputs msg;
        std::chrono::steady_clock::time_point deliver_at;
    };
    std::deque<DelayedMessage> delayQueue_;
};

} // namespace caster::dll::netplay
