// src/dll/spec/spectator_manager.cpp
//
// Phase C / Fase 2 — Host-side spectator manager (implementation).
//
// Issue #5 REDESIGN — "delayed spectation over proven truth":
//
//   - SpectatorManager keeps its OWN session archive: an append-only
//     vector of BothInputs batches pulled from the netplay containers
//     AFTER they crossed a safety lag behind the live edge (frames the
//     rollback engine can no longer rewrite). Per-transition-index
//     RngState and MenuIndex messages are archived too.
//
//   - Archiving runs every host frame from session start, regardless of
//     whether anyone is spectating (~250 KB/hour). A spectator joining
//     at ANY moment can replay from the very beginning.
//
//   - Promotion sends SpectateConfig + the whole archive dump (RngStates
//     first — the client defers application until playback crosses each
//     index — then every BothInputs batch). Serving is paced: burst while
//     the spectator is deep in backlog, throttled near the live tail.
//
//   - Spectator playback speed (fast-forward through the backlog, SPACE
//     toggle, auto-normal-on-live) is decided entirely client-side
//     (dll_main.cpp); the host just guarantees a complete, ordered,
//     proven-truth stream.
//
// Threading summary:
//   - onSpectatorConnect/Disconnect, step, tryPopOut: network thread only
//   - archiveStep, promotePending, frameStepSpectators,
//     archiveRngState/archiveMenuIndex, pushSyncHash: game thread
//   - All shared state sits behind _outMutex (single coarse mutex; with
//     MAX_ROOT_SPECTATORS=1 contention is non-existent).

#include "spectator_manager.hpp"
#include "../netplay/manager.hpp"
#include "../common/logger.hpp"

#ifndef NOMINMAX
#  define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <algorithm>
#include <chrono>

namespace caster::dll::spec {

namespace {

// Safety lag behind the live edge before a batch is considered proven
// (4 index-batches ≈ 2 seconds at 60fps).
constexpr std::uint32_t kArchiveSafetyLagFrames = 4 * NUM_INPUTS;

// While a spectator's cursor is deeper than this many batches from the
// archive end, it gets BURST service (initial catch-up).
constexpr std::size_t kLiveTailBatches = 4;

// Max batches served per spectator per frameStep in burst mode.
constexpr std::size_t kBurstBatches = 48;

// Batches served per spectator per frameStep near the live tail. Two
// 30-frame batches per 60fps tick = 2× realtime inflow, enough to hold
// position against jitter without racing ahead of proven truth.
constexpr std::size_t kTailBatchesPerTick = 2;

// Helper: get current tick count in ms (wraps GetTickCount for testability).
std::uint32_t now_ms() { return GetTickCount(); }

} // namespace

SpectatorManager::SpectatorManager(NetplayManager* netManPtr)
    : _netManPtr(netManPtr)
{
}

// ============================================================================
// Network thread: connect / disconnect / step
// ============================================================================

void SpectatorManager::onSpectatorConnect(ENetPeer* peer) {
    std::lock_guard<std::mutex> lock(_outMutex);

    if (_spectatorMap.find(peer) != _spectatorMap.end()) {
        common::logger::warn("spectator_manager: peer already active (double connect?)");
        return;
    }
    if (_pending.find(peer) != _pending.end()) {
        common::logger::warn("spectator_manager: peer already pending (double connect?)");
        return;
    }

    Spectator s;
    s.peer = peer;
    s.connectTick = now_ms();
    s.lastActivityTick = s.connectTick;
    _pending[peer] = s;

    common::logger::info(
        "spectator_manager: peer connected, pending promotion "
        "(archive={} batches)", _archive.size());
}

void SpectatorManager::onSpectatorDisconnect(ENetPeer* peer) {
    std::lock_guard<std::mutex> lock(_outMutex);

    if (_pending.erase(peer) > 0) {
        common::logger::info("spectator_manager: pending spectator disconnected");
        return;
    }
    if (_spectatorMap.erase(peer) > 0) {
        common::logger::info("spectator_manager: active spectator disconnected");
    }
}

void SpectatorManager::step() {
    std::lock_guard<std::mutex> lock(_outMutex);

    if (_pending.empty()) return;

    const std::uint32_t now = now_ms();
    std::vector<ENetPeer*> expired;
    for (auto& [peer, s] : _pending) {
        if (now - s.connectTick >= DEFAULT_PENDING_TIMEOUT_MS) {
            expired.push_back(peer);
        }
    }

    // Expired pending spectators simply fall out of _pending; their ENet
    // peer is closed by their own client-side timeout.
    for (ENetPeer* peer : expired) {
        common::logger::info("spectator_manager: pending spectator timed out, dropping");
        _pending.erase(peer);
    }
}

// ============================================================================
// Game thread: archive
// ============================================================================

void SpectatorManager::archiveStep() {
    if (!_netManPtr) return;
    std::lock_guard<std::mutex> lock(_outMutex);

    // Pull every batch that has crossed the safety lag. getBothInputs
    // advances _archivePos and returns nullopt once we're inside the lag
    // window (or the containers are empty — e.g. PreInitial frames before
    // any input was written).
    constexpr int kMaxPullsPerFrame = 8;
    for (int i = 0; i < kMaxPullsPerFrame; ++i) {
        auto bi = _netManPtr->getBothInputs(_archivePos, kArchiveSafetyLagFrames);
        if (!bi) break;
        _archive.push_back(std::move(*bi));
    }

    // CRITICAL (issue #5): the archiver is itself a history consumer.
    // Pin the netplay GC at our walk cursor — otherwise the containers
    // trim unarchived frames at every state transition and the archive
    // freezes the moment the host changes state (observed: spectator
    // starved at idx=1 frm=130 while players raced ahead to idx=8).
    _netManPtr->preserveStartIndex =
        std::min(_netManPtr->preserveStartIndex, _archivePos.parts.index);
}

void SpectatorManager::archiveRngState(std::uint32_t index, const RngState& rs) {
    std::lock_guard<std::mutex> lock(_outMutex);
    _rngArchive[index] = rs;

    // Forward live to already-connected spectators. The client defers
    // application until its playback crosses `index`.
    std::vector<std::uint8_t> bytes = rs.serialize();
    for (auto& [peer, s] : _spectatorMap) {
        enqueueOut({peer, bytes, /*reliable=*/true});
    }
}

void SpectatorManager::archiveMenuIndex(std::uint32_t index, const MenuIndex& mi) {
    std::lock_guard<std::mutex> lock(_outMutex);
    _menuArchive[index] = mi;

    std::vector<std::uint8_t> bytes = mi.serialize();
    for (auto& [peer, s] : _spectatorMap) {
        enqueueOut({peer, bytes, /*reliable=*/true});
    }
}

void SpectatorManager::pushSyncHash(const SyncHash& sh) {
    std::lock_guard<std::mutex> lock(_outMutex);
    if (_spectatorMap.empty()) return;
    std::vector<std::uint8_t> bytes = sh.serialize();
    for (auto& [peer, s] : _spectatorMap) {
        enqueueOut({peer, bytes, /*reliable=*/true});
    }
}

// ============================================================================
// Game thread: promotion
// ============================================================================

std::size_t SpectatorManager::promoteAllPending() {
    std::vector<ENetPeer*> pending_peers;
    {
        std::lock_guard<std::mutex> lock(_outMutex);
        pending_peers.reserve(_pending.size());
        for (auto& [peer, s] : _pending) pending_peers.push_back(peer);
    }

    std::size_t promoted = 0;
    for (ENetPeer* peer : pending_peers) {
        if (promotePending(peer)) ++promoted;
    }
    return promoted;
}

bool SpectatorManager::promotePending(ENetPeer* peer) {
    if (!_netManPtr) return false;

    std::lock_guard<std::mutex> lock(_outMutex);

    auto pit = _pending.find(peer);
    if (pit == _pending.end()) {
        common::logger::warn("spectator_manager: promotePending — peer not in pending");
        return false;
    }

    Spectator s = pit->second;
    _pending.erase(pit);

    if (_spectatorMap.size() >= MAX_ROOT_SPECTATORS) {
        common::logger::warn("spectator_manager: MAX_ROOT_SPECTATORS reached ({})",
                             MAX_ROOT_SPECTATORS);
        return false;
    }

    // Replay-from-start: the cursor begins at the first archived batch.
    s.archiveCursor = 0;
    _spectatorMap[peer] = s;

    common::logger::info(
        "spectator_manager: promoted — dumping archive ({} batches, {} rng, {} menus)",
        _archive.size(), _rngArchive.size(), _menuArchive.size());

    // 1. Match configuration (names, delay, win count...).
    SpectateConfig sc;
    sc.delay = _netManPtr->config.delay;
    sc.rollback = _netManPtr->config.rollback;
    sc.rollbackDelay = _netManPtr->config.rollbackDelay;
    sc.winCount = _netManPtr->config.winCount;
    sc.hostPlayer = _netManPtr->config.hostPlayer;
    sc.isTraining = _netManPtr->config.mode.isTraining() ? 1 : 0;
    sc.names = _netManPtr->config.names;
    enqueueOut({peer, sc.serialize(), /*reliable=*/true});

    // 2. Control history, ordered by transition index. The client defers
    //    application until its playback crosses each index, so sending
    //    these before the input batches is safe.
    for (auto& [idx, rs] : _rngArchive) {
        enqueueOut({peer, rs.serialize(), /*reliable=*/true});
    }
    for (auto& [idx, mi] : _menuArchive) {
        enqueueOut({peer, mi.serialize(), /*reliable=*/true});
    }

    // 3. Input batches are NOT pre-dumped — frameStepSpectators() serves
    //    them from _archive starting at the fresh spectator's cursor 0,
    //    in burst mode, so the link isn't saturated by one giant enqueue.

    return true;
}

// ============================================================================
// Game thread: serving
// ============================================================================

void SpectatorManager::frameStepSpectators() {
    if (!_netManPtr) return;
    std::lock_guard<std::mutex> lock(_outMutex);

    const std::size_t end = _archive.size();

    for (auto& [peer, s] : _spectatorMap) {
        if (s.archiveCursor >= end) continue;   // fully caught up

        const std::size_t backlog = end - s.archiveCursor;
        const std::size_t budget = (backlog > kLiveTailBatches)
                                       ? kBurstBatches
                                       : kTailBatchesPerTick;

        std::size_t served = 0;
        while (served < budget && s.archiveCursor < end) {
            const BothInputs& bi = _archive[s.archiveCursor];
            enqueueOut({peer, bi.serialize(), /*reliable=*/true});
            ++s.archiveCursor;
            ++served;
        }
        // No per-tick logging here — during catch-up this fires up to
        // 60x/s on the game thread.
    }
}

// ============================================================================
// Queries
// ============================================================================

std::size_t SpectatorManager::numSpectators() const {
    std::lock_guard<std::mutex> lock(_outMutex);
    return _spectatorMap.size();
}

std::size_t SpectatorManager::numPending() const {
    std::lock_guard<std::mutex> lock(_outMutex);
    return _pending.size();
}

std::string SpectatorManager::getRandomSpectatorAddress() const {
    // Phase C / Fase 5 — relay chaining not implemented yet.
    return {};
}

// ============================================================================
// Outbox (game thread pushes, network thread pops)
// ============================================================================

void SpectatorManager::enqueueOut(OutPacket pkt) {
    // Caller already holds _outMutex.
    _outQueue.push_back(std::move(pkt));
}

bool SpectatorManager::tryPopOut(OutPacket& out) {
    std::lock_guard<std::mutex> lock(_outMutex);
    if (_outQueue.empty()) return false;
    out = std::move(_outQueue.front());
    _outQueue.erase(_outQueue.begin());
    return true;
}

} // namespace caster::dll::spec
