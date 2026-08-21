// src/dll/spec/spectator_manager.hpp
//
// Phase C / Fase 2 — Host-side spectator manager.
//
// Ported from CCCaster's SpectatorManager (netplay/SpectatorManager.{hpp,cpp}
// + targets/DllSpectatorManager.cpp, ~389 LOC total), adapted for the
// ReCaster Layer 4 architecture:
//
//   - Runs on the **network thread** (Layer 4) — accept + broadcast
//     happen without blocking the game thread.
//   - No Timer/EventManager — uses GetTickCount() for the pending-socket
//     timeout.
//   - No Socket* — uses ENetPeer* (the NetworkThread owns the ENetHost*
//     and dispatches connect/receive/disconnect events).
//   - No mutexes for spectator state — the network thread is the sole
//     owner. The game thread accesses spectator state via the
//     SpectatorManager's public API, which is called from
//     NetworkThread::loop().
//   - No SpectateBroadcast mode (cut from v1, nobody used it in CCCaster).
//
// Spectator protocol (host-side perspective):
//   1. Spectator connects via ENet → NetworkThread emits CONNECT event
//      → SpectatorManager::onSpectatorConnect(peer)
//   2. Spectator is added to _pendingSpectators with a 20s timeout
//   3. Host calls SpectatorManager::promoteSpectator(peer) when ready
//      → sends SpectateConfig + InitialGameState + RngState
//      → moves to _spectatorMap
//   4. Each frameStep, host calls frameStepSpectators() which does
//      round-robin broadcast of BothInputs to 1+ spectators per frame
//      (interval based on spectator count)
//   5. Spectator disconnects → NetworkThread emits DISCONNECT event
//      → SpectatorManager::onSpectatorDisconnect(peer)
//
// Threading: ALL methods are called from the network thread. The game
// thread calls frameStepSpectators() via a thread-safe indirection
// (the NetworkThread exposes a stepSpectators() method that the game
// thread can invoke, which acquires the spectator mutex internally).
//
// Actually — re-reading the design: the simplest correct model is that
// frameStepSpectators() is called from the network thread's loop, NOT
// the game thread. The game thread just signals "ready for next
// broadcast" via an atomic flag. This keeps all spectator state on the
// network thread.

#pragma once

#include "../protocol/messages.hpp"
#include "../game/addresses.hpp"

#include <cstdint>
#include <map>
#include <mutex>
#include <unordered_map>
#include <vector>

// Forward declarations to avoid pulling ENet headers into this header.
struct _ENetPeer;
typedef struct _ENetPeer ENetPeer;

namespace caster::dll {

class NetplayManager;

namespace spec {

// Max spectators when the local client IS a spectator (relay chain).
inline constexpr std::size_t MAX_SPECTATORS = 15;

// Max spectators when the local client is the host/player (root).
// The host can only forward to 1 spectator directly; that spectator
// can then relay to others. This keeps host bandwidth bounded.
inline constexpr std::size_t MAX_ROOT_SPECTATORS = 1;

// Pending spectator timeout (ms). A spectator that connects but doesn't
// get promoted within this window is disconnected.
inline constexpr std::uint32_t DEFAULT_PENDING_TIMEOUT_MS = 20000;

// A connected spectator. Owned by SpectatorManager, accessed only from
// the network thread... and promoted/served from the game thread (all
// serving state lives under _outMutex).
struct Spectator {
    ENetPeer*    peer = nullptr;

    // Issue #5 redesign: spectators consume the session ARCHIVE (proven-
    // truth input history) from the very beginning, then ride the live
    // tail. cursor = next _archive entry to send. Replay-from-start is
    // the default; catch-up speed comes from the spectator's own
    // fast-forward, not from skipping archive entries.
    std::size_t  archiveCursor = 0;

    std::uint32_t connectTick = 0;     // for pending timeout
    std::uint32_t lastActivityTick = 0;
};

class SpectatorManager {
public:
    // netMan is accessed via friend access (getBothInputs, getRngState,
    // getState, getRetryMenuIndex, preserveStartIndex). The pointer is
    // stored but not owned.
    //
    // IMPORTANT: netMan methods are NOT thread-safe to call from the
    // network thread. SpectatorManager::frameStepSpectators() is
    // therefore called from the GAME THREAD (via a NetworkThread
    // indirection), not from the network thread loop. The spectator
    // state itself (the map/list) is protected by an internal mutex.
    explicit SpectatorManager(NetplayManager* netManPtr);
    ~SpectatorManager() = default;

    SpectatorManager(const SpectatorManager&)            = delete;
    SpectatorManager& operator=(const SpectatorManager&) = delete;

    // ---- Called from NetworkThread (network thread) ----

    // A new ENet peer connected. Add to pending spectators with the
    // 20s timeout. The peer will be promoted to active spectator (or
    // disconnected) by either promotePending() or the timeout check
    // in step().
    void onSpectatorConnect(ENetPeer* peer);

    // An ENet peer disconnected. Remove from pending or active
    // spectator map.
    void onSpectatorDisconnect(ENetPeer* peer);

    // Called every NetworkThread loop iteration. Checks for pending
    // spectator timeouts and disconnects expired ones.
    void step();

    // ---- Called from frameStep (game thread) ----

    // Issue #5 archive: pull every batch whose frames have crossed the
    // safety lag behind the live edge ("proven truth" — the rollback
    // engine can no longer rewrite them). Runs every frame on the host,
    // regardless of whether anyone is spectating, so a spectator joining
    // at ANY point can replay from session start.
    void archiveStep();

    // Promote a pending spectator: send SpectateConfig, then dump the
    // whole session archive (RngStates by index + every proven BothInputs
    // batch). The spectator's own fast-forward decides playback speed;
    // serving is paced so the dump doesn't saturate the link.
    //
    // Called from the game thread.
    bool promotePending(ENetPeer* peer);

    // Phase C / Fase 4: auto-promote ALL pending spectators. Called by
    // frameStep each frame. Returns the number of spectators promoted.
    std::size_t promoteAllPending();

    // Serve archived batches to every spectator: burst mode while a
    // spectator is deep in backlog (initial catch-up), paced near the
    // live tail. Called every frameStep from the game thread.
    void frameStepSpectators();

    // Archive a control message produced by the host (also forwarded
    // live to already-connected spectators).
    void archiveRngState(std::uint32_t index, const RngState& rs);
    void archiveMenuIndex(std::uint32_t index, const MenuIndex& mi);

    // ---- Queries ----

    std::size_t numSpectators() const;
    std::size_t numPending() const;

    // Called by NetplayManager when a new RngState is generated. Forwards
    // to all active spectators.
    void newRngState(const RngState& rngState);

    // Issue #5: forward the host's own SyncHash to all active spectators
    // so they can run desync detection against the replayed stream.
    void pushSyncHash(const SyncHash& sh);

    // Get a random spectator's "server address" for redirect when full.
    // Returns empty string if no spectators or no spectator has a
    // non-zero address. (Phase C / Fase 5 — relay spectate — uses this.)
    std::string getRandomSpectatorAddress() const;

    // Queue an outgoing packet for a specific spectator. The NetworkThread
    // drains this queue in its loop. Thread-safe (acquires internal
    // mutex). Used by promotePending() and frameStepSpectators().
    struct OutPacket {
        ENetPeer*              peer = nullptr;
        std::vector<std::uint8_t> bytes;
        bool                   reliable = true;
    };
    void enqueueOut(OutPacket pkt);
    bool tryPopOut(OutPacket& out);

private:
    NetplayManager* _netManPtr = nullptr;

    // Pending spectators: connected but not yet promoted. Keyed by peer.
    std::unordered_map<ENetPeer*, Spectator> _pending;

    // Active spectators: promoted, consuming the session archive.
    std::unordered_map<ENetPeer*, Spectator> _spectatorMap;

    // Issue #5 session archive — append-only proven-truth history,
    // independent of the netplay containers' garbage collection. The
    // host appends every frame (archiveStep), so a spectator joining at
    // ANY moment can replay from session start.
    std::vector<BothInputs> _archive;

    // Walk cursor into the netplay containers for archiveStep()'s pulls
    // (same space as the containers' IndexedFrame).
    IndexedFrame _archivePos = {{0, 0}};

    // Per-transition-index control messages, archived so late joiners
    // replay them at the right moment (the client defers application
    // until its playback crosses the index).
    std::map<std::uint32_t, RngState>  _rngArchive;
    std::map<std::uint32_t, MenuIndex> _menuArchive;

    // Outgoing packet queue. Drained by NetworkThread::loop().
    mutable std::mutex _outMutex;
    std::vector<OutPacket> _outQueue;
};

} // namespace spec
} // namespace caster::dll
