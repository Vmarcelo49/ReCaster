// src/dll/netplay/network_thread.hpp
//
// Layer 4 — Network thread foundation (subtask 4.1).
//
// The NetworkThread owns the ENetHost* and runs a dedicated jthread that
// calls enet_host_service() in a loop. This is the single thread that
// touches ENet — the game thread communicates exclusively via the inbox
// BlockingQueues (Network → Game) and the outbox queue (Game → Network).
//
// In subtask 4.1, this is scaffolding only:
//   - The class exists and can be constructed/started/stopped
//   - It owns an ENetHost* (created in start(), destroyed in stop())
//   - The jthread loop runs but only logs connection events — no inbox
//     routing yet (that's subtask 4.3)
//   - The 5 inbox BlockingQueues exist as members but aren't populated
//     yet (subtask 4.3 routes ENET_EVENT_TYPE_RECEIVE into them)
//   - The outbox queue exists but send() isn't wired yet (subtask 4.3)
//
// This is intentionally minimal: validate that jthread + stop_token +
// ENet initialization work together in the DLL context before moving
// real code in. Once 4.1 builds clean, 4.2 adds NetworkSimulator, 4.3
// moves the actual receive/send logic from connector.cpp.
//
// Threading model:
//   - Construction/destruction: caller's thread (game thread)
//   - start(): caller's thread — initializes ENet, spawns the jthread
//   - stop(): caller's thread — requests stop, joins the jthread,
//     destroys ENetHost. MUST complete before the caller returns.
//   - loop() body: the network jthread only
//   - Inbox queues: SPSC — network thread is sole producer, game thread
//     is sole consumer
//   - Outbox queue: SPSC — game thread is sole producer, network thread
//     is sole consumer
//
// Design decisions (see docs/threading-migration.md Layer 4):
//   - Decision 2: 5 separate BlockingQueue<T> (one per message type)
//   - Decision 3: NetworkSimulator is a separate class (subtask 4.2),
//     owned by NetworkThread as a member
//   - The 5 inboxes are public read-only accessors for the connector
//     facade to use in subtask 4.4

#pragma once

#include "network_simulator.hpp"
#include "protocol/messages.hpp"
#include "../../common/concurrency.hpp"
#include "../../common/ipc/config_buffer.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>

// Forward declarations to avoid pulling ENet headers into this header.
// ENet types are only needed in the .cpp.
struct _ENetHost;
typedef struct _ENetHost ENetHost;
struct _ENetPeer;
typedef struct _ENetPeer ENetPeer;

// Forward declaration at top level (outside caster::dll::netplay) to
// avoid namespace-nesting ambiguity. SpectatorManager lives in
// caster::dll::spec, not caster::dll::netplay::spec.
namespace caster::dll::spec { class SpectatorManager; }

namespace caster::dll::netplay {

// A pending outgoing packet. The game thread enqueues these; the network
// thread drains them inside its loop and calls enet_peer_send.
//
// `reliable` controls the ENet packet flag:
//   - PlayerInputs: UNRELIABLE (per-frame, drop is ok)
//   - All control messages: RELIABLE (must arrive)
//
// `peer` is optional — when nullptr, the packet goes to the primary peer_
// (the netplay opponent). When set, it goes to a specific spectator peer.
// This is used by SpectatorManager::tryPopOut() to address individual
// spectators.
struct OutboxEntry {
    std::vector<uint8_t> bytes;
    bool                 reliable = false;
    void*                peer = nullptr;  // ENetPeer* — nullptr = primary peer
};

class NetworkThread {
public:
    NetworkThread() = default;
    ~NetworkThread();

    NetworkThread(const NetworkThread&)            = delete;
    NetworkThread& operator=(const NetworkThread&) = delete;
    NetworkThread(NetworkThread&&)                 = delete;
    NetworkThread& operator=(NetworkThread&&)      = delete;

    // ---- Lifecycle ----

    // Initialize ENet, create the ENetHost bound to cfg.local_udp_port,
    // (if client) issue enet_host_connect to cfg.peer_addr:cfg.peer_port,
    // and spawn the worker jthread.
    //
    // Idempotent: calling start() twice is a no-op (the second call logs
    // and returns). For offline modes (cfg.is_netplay() == false), this
    // is a no-op — the NetworkThread exists but has no ENet host and no
    // jthread.
    //
    // MUST be called from the game thread.
    void start(const caster::common::ipc::config_buffer::Config& cfg);

    // Signal the jthread to stop, join it, disconnect the peer, and
    // destroy the ENetHost. Safe to call multiple times.
    //
    // MUST be called from the game thread (the same thread that called
    // start()).
    //
    // CRITICAL: this MUST complete (jthread joined + ENetHost destroyed)
    // before the caller returns, because the DLL may unhook game code
    // immediately after. The jthread auto-joins on destruction as a
    // safety net, but explicit stop() in DLL_PROCESS_DETACH is required
    // for clean ordering.
    void stop();

    // True if start() was called and the jthread is running.
    bool isRunning() const { return thread_.joinable(); }

    // True once the ENet peer has connected (handshake completed).
    // Atomic — safe to read from any thread. Used by the connector
    // facade's connected() in subtask 4.4.
    bool connected() const { return connected_.load(std::memory_order_acquire); }

    // Mirror of cfg.is_host() at start time. False for offline.
    bool isHost() const { return isHost_; }

    // ---- Inbox accessors (Network → Game) ----
    //
    // The game thread drains these via try_pop() in drainNetplayInbox().
    // The network thread is the sole producer.
    //
    // Each queue is a BlockingQueue<T> from src/common/concurrency.hpp.
    // Use try_pop() (non-blocking) from the game thread; the network
    // thread uses push() (non-blocking, notifies the internal cv but
    // nobody is wait_and_pop'ing in v1).

    caster::common::concurrency::BlockingQueue<PlayerInputs>& inboxPlayerInputs() {
        return inboxPlayerInputs_;
    }
    caster::common::concurrency::BlockingQueue<uint32_t>& inboxTransitionIndex() {
        return inboxTransitionIndex_;
    }
    caster::common::concurrency::BlockingQueue<MenuIndex>& inboxMenuIndex() {
        return inboxMenuIndex_;
    }
    caster::common::concurrency::BlockingQueue<RngState>& inboxRngState() {
        return inboxRngState_;
    }
    caster::common::concurrency::BlockingQueue<SyncHash>& inboxSyncHash() {
        return inboxSyncHash_;
    }

    // ---- Spectator-only inboxes (Phase C / Fase 3) ----
    //
    // These are only populated when the local client is a spectator.
    // The game thread drains them via drainNetplayInbox() and forwards
    // to SpectateClient. Host-side never touches these.
    caster::common::concurrency::BlockingQueue<BothInputs>& inboxBothInputs() {
        return inboxBothInputs_;
    }
    caster::common::concurrency::BlockingQueue<InitialGameState>& inboxInitialGameState() {
        return inboxInitialGameState_;
    }
    caster::common::concurrency::BlockingQueue<SpectateConfig>& inboxSpectateConfig() {
        return inboxSpectateConfig_;
    }

    // ---- Outbox (Game → Network) ----
    //
    // The game thread pushes OutboxEntry via enqueueOutbox(); the network
    // thread drains it inside its loop and calls enet_peer_send.
    void enqueueOutbox(OutboxEntry entry) {
        outbox_.push(std::move(entry));
    }

    // ---- Spectator manager (Phase C / Fase 2.5) ----
    //
    // Optional SpectatorManager owned by the caller (dll_main). When set,
    // the network thread dispatches CONNECT/DISCONNECT events to it and
    // drains its outbox in every loop iteration. The SpectatorManager
    // handles pending timeouts via step().
    //
    // MUST be set BEFORE start() spawns the jthread. Set to nullptr to
    // disable spectator support (e.g. client-side, or offline mode).
    void setSpectatorManager(caster::dll::spec::SpectatorManager* sm) { spectatorMgr_ = sm; }
    caster::dll::spec::SpectatorManager* spectatorManager() { return spectatorMgr_; }

private:
    // ---- ENet state (network thread owns, but start/stop touch from game thread) ----
    //
    // The ENetHost* and ENetPeer* are created in start() (game thread)
    // and destroyed in stop() (game thread). The network thread reads
    // them inside loop() but never destroys them — that's stop()'s job,
    // after the jthread has joined.
    //
    // This is safe because:
    //   - start() spawns the jthread AFTER host_ is fully initialized
    //   - stop() requests stop, joins the jthread, THEN destroys host_
    //   - The jthread only reads host_/peer_ inside loop(), and once
    //     stop is requested, loop() exits its iteration
    ENetHost* host_ = nullptr;
    ENetPeer* peer_ = nullptr;
    std::atomic<bool> connected_{false};
    bool     isNetplay_ = false;
    bool     isHost_    = false;
    uint16_t localPort_ = 0;
    std::string peerAddr_;
    uint16_t peerPort_  = 0;

    // ---- Worker jthread ----
    std::jthread thread_;

    // ---- Inbox queues (Network → Game) ----
    //
    // 5 separate BlockingQueue<T>, one per message type. See Decision 2
    // in docs/threading-migration.md. Each is SPSC: network thread
    // produces, game thread consumes.
    caster::common::concurrency::BlockingQueue<PlayerInputs> inboxPlayerInputs_;
    caster::common::concurrency::BlockingQueue<uint32_t>     inboxTransitionIndex_;
    caster::common::concurrency::BlockingQueue<MenuIndex>    inboxMenuIndex_;
    caster::common::concurrency::BlockingQueue<RngState>     inboxRngState_;
    caster::common::concurrency::BlockingQueue<SyncHash>     inboxSyncHash_;

    // Phase C / Fase 3: spectator-only inboxes. Populated only when the
    // local client is a spectator.
    caster::common::concurrency::BlockingQueue<BothInputs>       inboxBothInputs_;
    caster::common::concurrency::BlockingQueue<InitialGameState> inboxInitialGameState_;
    caster::common::concurrency::BlockingQueue<SpectateConfig>   inboxSpectateConfig_;

    // ---- Outbox queue (Game → Network) ----
    caster::common::concurrency::BlockingQueue<OutboxEntry>  outbox_;

    // ---- Network simulator (subtask 4.2) ----
    //
    // Owned by NetworkThread, accessed only by the network thread after
    // start() returns. configure() is called once in start() before the
    // jthread is spawned — establishes happens-before.
    NetworkSimulator sim_;

    // ---- Spectator manager (Phase C / Fase 2.5) ----
    //
    // NOT owned — set by the caller (dll_main) via setSpectatorManager()
    // before start() spawns the jthread. May be nullptr (client-side,
    // offline, or when spectator support is disabled).
    caster::dll::spec::SpectatorManager* spectatorMgr_ = nullptr;

    // ---- Worker loop (runs on the jthread) ----
    //
    // Subtask 4.1: minimal loop — just enet_host_service() in a 10ms
    // loop, logging connect/disconnect/receive events. No inbox routing
    // yet (subtask 4.3).
    //
    // Subtask 4.3 will extend this to:
    //   - Decode received packets and push to the matching inbox
    //   - Drain outbox_ and call enet_peer_send
    //   - Run the NetworkSimulator's deliverExpired() + apply loss/delay
    //     to PlayerInputs
    void loop(std::stop_token st);
};

} // namespace caster::dll::netplay
