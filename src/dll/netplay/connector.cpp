// src/dll/netplay/connector.cpp
//
// Layer 4 — Network thread foundation (subtask 4.4).
//
// This file is now a THIN FACADE over NetworkThread. All ENet I/O has
// moved to NetworkThread (subtask 4.3); connector.cpp exists only to
// preserve the public API that dll_main.cpp depends on.
//
// What's here:
//   - A global NetworkThread instance (g_networkThread)
//   - start(cfg)     → g_networkThread.start(cfg)
//   - shutdown()     → g_networkThread.stop()
//   - connected()    → g_networkThread.connected()
//   - isHost()       → g_networkThread.isHost()
//   - poll()         → no-op (the network thread polls itself now)
//   - send*(msg)     → g_networkThread.enqueueOutbox({msg.serialize(), reliable})
//   - recv*()        → g_networkThread.inbox*().try_pop()
//
// What's NOT here anymore (moved to NetworkThread):
//   - g_host, g_peer, g_connected, g_isHost, g_localPort, etc.
//   - The 5 inbox std::queue<T>
//   - sendPacket()
//   - The network simulator (g_sim, g_delayQueue, deliverExpiredDelayed)
//   - The poll() body that called enet_host_service()
//
// See docs/threading-migration.md Layer 4 for the design.

#include "connector.hpp"
#include "network_thread.hpp"
#include "thread_affinity.hpp"
#include "../spec/spectator_manager.hpp"
#include "../common/logger.hpp"

#include <memory>
#include <optional>
#include <utility>

namespace caster::dll::netplay {

namespace {

// The single NetworkThread instance. Owned by this TU, lives for the
// lifetime of the DLL. Allocated lazily in start() to avoid
// static-initialization order issues with the logger.
//
// Why std::unique_ptr and not a static local? Because the
// NetworkThread's destructor joins its jthread, and we want that join
// to happen at an explicit point (in shutdown(), called from
// DLL_PROCESS_DETACH) — not at program exit when the static-local
// destructor runs (which might be after the ENet library has been
// deinitialized by something else).
std::unique_ptr<NetworkThread> g_networkThread;

// Phase C / Fase 2.5: SpectatorManager, owned by this TU. Only created
// when the local client is the host (only hosts accept spectators).
// Lives for the duration of the netplay session.
std::unique_ptr<caster::dll::spec::SpectatorManager> g_spectatorMgr;

// True once start() was called. Used to distinguish "start() never
// called" from "start() called but offline mode (no jthread)" in
// shutdown().
bool g_started = false;

} // namespace

// ============================================================================
// Lifecycle
// ============================================================================

void start(const caster::common::ipc::config_buffer::Config& cfg) {
    if (g_started) {
        common::logger::warn("connector: start() called twice — ignoring");
        return;
    }
    g_started = true;

    g_networkThread = std::make_unique<NetworkThread>();

    // Phase C / Fase 2.5: create SpectatorManager for the host. The
    // manager needs a NetplayManager pointer, but we don't have one
    // here — the caller (dll_main) sets it via setSpectatorNetMan()
    // before start() is called. For now, create with nullptr and let
    // dll_main wire it up after.
    //
    // Actually — re-reading the design: SpectatorManager::frameStepSpectators()
    // and promotePending() need the NetplayManager. We can't create the
    // SpectatorManager here without it. So the creation is deferred to
    // dll_main, which calls connector::initSpectatorManager(netMan).
    // For now, g_spectatorMgr stays nullptr here.
    //
    // The NetworkThread will check spectatorMgr_ == nullptr and skip
    // spectator handling. Once dll_main creates the SpectatorManager
    // and calls setSpectatorManager() on the NetworkThread, the loop
    // will start dispatching connect/disconnect events to it.
    //
    // IMPORTANT: setSpectatorManager() must be called BEFORE the
    // NetworkThread starts accepting connections. Since the jthread
    // is spawned in NetworkThread::start() below, dll_main must call
    // connector::initSpectatorManager() AFTER connector::start() but
    // BEFORE any spectator could possibly connect. In practice this
    // is safe because spectators won't connect until the host is
    // listening (which happens inside NetworkThread::start()) — but
    // there's a tiny race window. The SpectatorManager handles this
    // gracefully: onSpectatorConnect() is called from the network
    // thread, and if spectatorMgr_ is still nullptr, the connection
    // is rejected with a warning. The spectator can retry.

    g_networkThread->start(cfg);
    // For offline mode, start() is a no-op (no ENet host, no jthread).
    // The facade functions below handle this by checking
    // g_networkThread->connected() / isHost() which return false for
    // offline.
}

// Phase C / Fase 2.5: create SpectatorManager with the given NetplayManager.
// Called by dll_main after NetplayManager is initialized and BEFORE
// spectators could connect. Only creates the manager if the local
// client is the host — clients and spectators don't accept spectators.
void initSpectatorManager(NetplayManager* netMan) {
    if (!g_started || !g_networkThread) return;
    if (!g_networkThread->isHost()) return;  // Only host accepts spectators
    if (g_spectatorMgr) return;  // Already initialized

    g_spectatorMgr = std::make_unique<caster::dll::spec::SpectatorManager>(netMan);
    g_networkThread->setSpectatorManager(g_spectatorMgr.get());
    common::logger::info("connector: SpectatorManager initialized for host");
}

void poll() {
    // No-op. The NetworkThread polls ENet itself on its dedicated jthread.
    //
    // Kept in the public API for source compatibility — dll_main.cpp
    // calls netplay::poll() in two places (top of frameStep and inside
    // the spin-lock gate). The 4.6 subtask removes those calls; for now
    // they remain as no-ops so the build stays green during the
    // incremental migration.
    //
    // If you're seeing this log line in production, it means we missed
    // removing a poll() call somewhere. (Subtask 4.6 removes them.)
    // common::logger::warn("connector: poll() is a no-op in Layer 4");
    // -- commented out to avoid log spam at 60Hz; uncomment when debugging.
}

void shutdown() {
    if (!g_started) return;

    // Phase C / Fase 2.5: clear the SpectatorManager pointer on the
    // NetworkThread BEFORE stopping it, so the network thread's loop
    // stops touching the SpectatorManager. Then destroy the manager.
    if (g_networkThread) {
        g_networkThread->setSpectatorManager(nullptr);
    }
    g_spectatorMgr.reset();

    if (g_networkThread) {
        g_networkThread->stop();
        g_networkThread.reset();
    }
    g_started = false;

    common::logger::info("connector: shut down");
}

// ============================================================================
// Connection state
// ============================================================================

bool connected() {
    return g_networkThread && g_networkThread->connected();
}

bool isHost() {
    return g_networkThread && g_networkThread->isHost();
}

// Phase C / Fase 2.5: SpectatorManager accessor.
caster::dll::spec::SpectatorManager* spectatorManager() {
    return g_spectatorMgr.get();
}

// ============================================================================
// Outbox (frameStep → peer)
// ============================================================================
//
// Each send*() serializes the message and enqueues an OutboxEntry to
// the NetworkThread's outbox queue. The network thread drains the
// outbox inside its loop and calls enet_peer_send + enet_host_flush.
//
// `reliable` controls the ENet packet flag:
//   - PlayerInputs: UNRELIABLE (per-frame, drop is ok)
//   - All control messages: RELIABLE (must arrive)

void sendPlayerInputs(const PlayerInputs& pi) {
    // PlayerInputs is the per-frame high-frequency message. We send it
    // UNRELIABLE+UNSEQUENCED so that if a packet is lost or delayed, the
    // next one (with a more recent indexedFrame) supersedes it. The
    // receiver's InputsContainer.assign() overwrites older frames.
    //
    // The NetplayManager always sends the last NUM_INPUTS frames, so a
    // single dropped packet is recovered by the next one (which still
    // contains the dropped frames in its window).
    //
    // Subtask 4.9: assert we're NOT holding NetplayManager::_mutex —
    // enqueueOutbox does BlockingQueue::push which under contention
    // may cv-wait, and waiting while holding the FSM lock is a deadlock
    // risk if the network thread needs the FSM lock to make progress.
    thread_affinity::check_not_holding_netman_mutex("netplay::sendPlayerInputs");
    if (!g_networkThread) return;
    g_networkThread->enqueueOutbox({pi.serialize(), /*reliable=*/false});
}

void sendTransitionIndex(uint32_t index) {
    thread_affinity::check_not_holding_netman_mutex("netplay::sendTransitionIndex");
    if (!g_networkThread) return;
    TransitionIndex ti(index);
    g_networkThread->enqueueOutbox({ti.serialize(), /*reliable=*/true});
}

void sendMenuIndex(const MenuIndex& mi) {
    thread_affinity::check_not_holding_netman_mutex("netplay::sendMenuIndex");
    if (!g_networkThread) return;
    // Retry menu selection sync — must arrive exactly once. RELIABLE.
    g_networkThread->enqueueOutbox({mi.serialize(), /*reliable=*/true});
}

void sendRngState(const RngState& rs) {
    thread_affinity::check_not_holding_netman_mutex("netplay::sendRngState");
    if (!g_networkThread) return;
    // RNG sync — must arrive (desync prevention). RELIABLE.
    g_networkThread->enqueueOutbox({rs.serialize(), /*reliable=*/true});
}

void sendSyncHash(const SyncHash& sh) {
    thread_affinity::check_not_holding_netman_mutex("netplay::sendSyncHash");
    if (!g_networkThread) return;
    // Desync detection — must arrive (we'd miss a desync if dropped).
    // RELIABLE.
    g_networkThread->enqueueOutbox({sh.serialize(), /*reliable=*/true});
}

// ============================================================================
// Inbox (peer → frameStep)
// ============================================================================
//
// Each recv*() does a non-blocking try_pop on the matching
// BlockingQueue<T> inside NetworkThread. Returns nullopt if the queue
// is empty (the caller's drain loop will just exit).

std::optional<PlayerInputs> recvPlayerInputs() {
    if (!g_networkThread) return std::nullopt;
    return g_networkThread->inboxPlayerInputs().try_pop();
}

std::optional<uint32_t> recvTransitionIndex() {
    if (!g_networkThread) return std::nullopt;
    return g_networkThread->inboxTransitionIndex().try_pop();
}

std::optional<MenuIndex> recvMenuIndex() {
    if (!g_networkThread) return std::nullopt;
    return g_networkThread->inboxMenuIndex().try_pop();
}

std::optional<RngState> recvRngState() {
    if (!g_networkThread) return std::nullopt;
    return g_networkThread->inboxRngState().try_pop();
}

std::optional<SyncHash> recvSyncHash() {
    if (!g_networkThread) return std::nullopt;
    return g_networkThread->inboxSyncHash().try_pop();
}

// Phase C / Fase 3: spectator-only inbox drains.
std::optional<BothInputs> recvBothInputs() {
    if (!g_networkThread) return std::nullopt;
    return g_networkThread->inboxBothInputs().try_pop();
}

std::optional<InitialGameState> recvInitialGameState() {
    if (!g_networkThread) return std::nullopt;
    return g_networkThread->inboxInitialGameState().try_pop();
}

std::optional<SpectateConfig> recvSpectateConfig() {
    if (!g_networkThread) return std::nullopt;
    return g_networkThread->inboxSpectateConfig().try_pop();
}

} // namespace caster::dll::netplay
