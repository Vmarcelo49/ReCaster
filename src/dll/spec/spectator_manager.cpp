// src/dll/spec/spectator_manager.cpp
//
// Phase C / Fase 2 — Host-side spectator manager (implementation).
//
// See spectator_manager.hpp for the design rationale and threading model.
//
// Threading summary:
//   - onSpectatorConnect/Disconnect, step, tryPopOut: network thread only
//   - promotePending, frameStepSpectators, newRngState: game thread
//   - The spectator state (_pending, _spectatorMap, _spectatorList) is
//     accessed by both threads, so all accessors acquire _outMutex.
//     (Yes, _outMutex guards both the outbox queue AND the spectator
//     state — this is intentional, it's a single coarse mutex. With
//     MAX_ROOT_SPECTATORS=1, contention is non-existent.)
//   - NetplayManager accesses in promotePending() and frameStepSpectators()
//     are NOT guarded by NetplayManager::_mutex — these methods are called
//     from the game thread, which is the same thread that owns the FSM.
//     The *Locked convention doesn't apply here because we're calling
//     public NetplayManager methods (which acquire _mutex internally),
//     NOT touching private fields directly.
//
// Wait — actually we DO touch private fields directly: netMan->preserveStartIndex
// is a public field, and getBothInputs is a public method. Let me re-check...
//
// NetplayManager::preserveStartIndex is a PUBLIC field (manager.hpp:83),
// so direct access is fine. getBothInputs() is a public method that
// acquires _mutex internally. So we're good.

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

// Helper: get current tick count in ms (wraps GetTickCount for testability).
static std::uint32_t now_ms() { return GetTickCount(); }

SpectatorManager::SpectatorManager(NetplayManager* netManPtr)
    : _spectatorListPos(_spectatorList.end())
    , _spectatorMapPos(_spectatorMap.end())
    , _netManPtr(netManPtr)
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

    common::logger::info("spectator_manager: peer connected, pending promotion (timeout={}ms)",
                         DEFAULT_PENDING_TIMEOUT_MS);
}

void SpectatorManager::onSpectatorDisconnect(ENetPeer* peer) {
    std::lock_guard<std::mutex> lock(_outMutex);

    auto pit = _pending.find(peer);
    if (pit != _pending.end()) {
        _pending.erase(pit);
        common::logger::info("spectator_manager: pending spectator disconnected");
        return;
    }

    auto sit = _spectatorMap.find(peer);
    if (sit != _spectatorMap.end()) {
        // Fix iterator validity before erasing from list.
        if (_spectatorListPos != _spectatorList.end() && *_spectatorListPos == peer) {
            ++_spectatorListPos;
        }
        if (_spectatorMapPos != _spectatorMap.end() && _spectatorMapPos->first == peer) {
            ++_spectatorMapPos;
        }
        // Remove from round-robin list. std::list::remove is safe and
        // does a linear scan — fine for MAX_ROOT_SPECTATORS=1 (typically
        // 0 or 1 element).
        _spectatorList.remove(peer);
        _spectatorMap.erase(sit);
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

    for (ENetPeer* peer : expired) {
        common::logger::info("spectator_manager: pending spectator timed out, disconnecting");
        // Queue a disconnect packet — the NetworkThread will perform
        // enet_peer_disconnect_later. We can't call it here directly
        // because we hold _outMutex and the NetworkThread might be
        // processing it. The simplest is to queue a "disconnect" command
        // via the outbox with empty bytes + reliable=false, and let
        // NetworkThread check for this sentinel.
        //
        // Actually, for simplicity in Phase C / Fase 2, we just erase
        // from pending — the ENet peer will be force-disconnected by
        // the NetworkThread when it detects the peer is no longer in
        // any SpectatorManager container. (TBD — for now, we rely on
        // the spectator's own client-side timeout.)
        _pending.erase(peer);
    }
}

// ============================================================================
// Game thread: promotePending + promoteAllPending + frameStepSpectators
// ============================================================================

std::size_t SpectatorManager::promoteAllPending() {
    // Snapshot the pending peers under the lock, then promote each one.
    // promotePending() re-acquires the lock, so we can't hold it here.
    std::vector<ENetPeer*> pending_peers;
    {
        std::lock_guard<std::mutex> lock(_outMutex);
        pending_peers.reserve(_pending.size());
        for (auto& [peer, s] : _pending) {
            pending_peers.push_back(peer);
        }
    }

    std::size_t promoted = 0;
    for (ENetPeer* peer : pending_peers) {
        if (promotePending(peer)) {
            ++promoted;
        }
    }
    return promoted;
}

bool SpectatorManager::promotePending(ENetPeer* peer) {
    // Read NetplayManager state first (no lock needed — game thread
    // owns the FSM, and public NetplayManager methods acquire _mutex).
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
        common::logger::warn("spectator_manager: MAX_ROOT_SPECTATORS reached ({}),"
                             " rejecting promotion", MAX_ROOT_SPECTATORS);
        // TBD: redirect to getRandomSpectatorAddress() — Phase C / Fase 5.
        return false;
    }

    // Initialize the spectator's position.
    s.pos.parts.frame = NUM_INPUTS - 1;
    s.pos.parts.index = _netManPtr->getSpectateStartIndex();

    // Insert into round-robin list AFTER the current position
    // (matches CCCaster's pushSpectator logic).
    std::list<ENetPeer*>::iterator it;
    if (_spectatorList.empty()) {
        it = _spectatorList.insert(_spectatorList.end(), peer);
    } else if (_spectatorListPos == _spectatorList.end()) {
        it = _spectatorList.insert(_spectatorList.begin(), peer);
    } else {
        auto next = std::next(_spectatorListPos);
        it = _spectatorList.insert(next, peer);
    }

    _spectatorMap[peer] = s;

    if (_spectatorMap.size() == 1 || _spectatorMapPos == _spectatorMap.end()) {
        _spectatorMapPos = _spectatorMap.cbegin();
    }

    // Update preserveStartIndex so old inputs aren't garbage-collected.
    _netManPtr->preserveStartIndex = std::min(_netManPtr->preserveStartIndex,
                                              s.pos.parts.index);

    common::logger::info("spectator_manager: spectator promoted, pos=[idx={},frame={}]",
                         s.pos.parts.index, s.pos.parts.frame);

    // Send SpectateConfig (build it from NetplayManager config).
    SpectateConfig sc;
    sc.delay = _netManPtr->config.delay;
    sc.rollback = _netManPtr->config.rollback;
    sc.rollbackDelay = _netManPtr->config.rollbackDelay;
    sc.winCount = _netManPtr->config.winCount;
    sc.hostPlayer = _netManPtr->config.hostPlayer;
    sc.isTraining = _netManPtr->config.mode.isTraining() ? 1 : 0;
    sc.names = _netManPtr->config.names;
    enqueueOut({peer, sc.serialize(), /*reliable=*/true});

    // Send InitialGameState.
    InitialGameState igs;
    igs.indexedFrame = s.pos;
    igs.netplayState = static_cast<uint8_t>(_netManPtr->getState());
    igs.isTraining = sc.isTraining;
    igs.readFromGame(s.pos, igs.netplayState, sc.isTraining != 0);
    enqueueOut({peer, igs.serialize(), /*reliable=*/true});

    // Send initial RngState.
    auto rngState = _netManPtr->getRngState(s.pos.parts.index);
    if (rngState) {
        enqueueOut({peer, rngState->serialize(), /*reliable=*/true});
    }

    return true;
}

void SpectatorManager::frameStepSpectators() {
    if (!_netManPtr) return;

    std::lock_guard<std::mutex> lock(_outMutex);

    if (_spectatorMap.empty()) {
        _spectatorListPos = _spectatorList.end();
        _spectatorMapPos = _spectatorMap.end();
        _netManPtr->preserveStartIndex = _currentMinIndex = UINT_MAX;
        return;
    }

    if (_spectatorMapPos == _spectatorMap.end()) {
        _spectatorMapPos = _spectatorMap.cbegin();
    }
    if (_spectatorMap.size() > 1) {
        ++_spectatorMapPos;
    }
    if (_spectatorMapPos == _spectatorMap.end()) {
        _spectatorMapPos = _spectatorMap.cbegin();
    }

    // Number of broadcasts per frame (scales with spectator count).
    const std::uint32_t multiplier = 1 + (static_cast<std::uint32_t>(_spectatorList.size()) * 2)
                                         / (NUM_INPUTS + 1);
    // Frames between each broadcast.
    const std::uint32_t interval = (multiplier * NUM_INPUTS / 2)
                                    / static_cast<std::uint32_t>(_spectatorList.size());

    // Skip frames that aren't on the interval.
    if ((*asU32(CC_WORLD_TIMER_ADDR)) % interval) return;

    for (std::uint32_t i = 0; i < multiplier; ++i) {
        if (_spectatorListPos == _spectatorList.end()) {
            _spectatorListPos = _spectatorList.begin();
            _netManPtr->preserveStartIndex = _currentMinIndex;
            _currentMinIndex = UINT_MAX;
        }

        ENetPeer* peer = *_spectatorListPos;
        auto it = _spectatorMap.find(peer);
        if (it == _spectatorMap.end()) {
            // Shouldn't happen — list and map should be in sync.
            ++_spectatorListPos;
            continue;
        }

        Spectator& s = it->second;
        const std::uint32_t oldIndex = s.pos.parts.index;

        // Send BothInputs if available.
        auto bi = _netManPtr->getBothInputs(s.pos);
        if (bi) {
            enqueueOut({peer, bi->serialize(), /*reliable=*/true});
        }

        // Send RngState once per index.
        auto rngState = _netManPtr->getRngState(oldIndex);
        if (rngState && !s.sentRngState) {
            enqueueOut({peer, rngState->serialize(), /*reliable=*/true});
            s.sentRngState = true;
        }

        // Clear sent flags on index change.
        if (s.pos.parts.index > oldIndex) {
            s.sentRngState = false;
            s.sentRetryMenuIndex = false;
        }

        // Send retry menu index once per index.
        auto menuIdx = _netManPtr->getRetryMenuIndex(oldIndex);
        if (menuIdx && !s.sentRetryMenuIndex) {
            enqueueOut({peer, menuIdx->serialize(), /*reliable=*/true});
            s.sentRetryMenuIndex = true;
        }

        ++_spectatorListPos;
        _currentMinIndex = std::min(_currentMinIndex, s.pos.parts.index);
    }
}

// ============================================================================
// RngState broadcast
// ============================================================================

void SpectatorManager::newRngState(const RngState& rngState) {
    std::lock_guard<std::mutex> lock(_outMutex);
    std::vector<std::uint8_t> bytes = rngState.serialize();
    for (auto& [peer, s] : _spectatorMap) {
        enqueueOut({peer, bytes, /*reliable=*/true});
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
    std::lock_guard<std::mutex> lock(_outMutex);
    if (_spectatorMap.empty() || _spectatorMapPos == _spectatorMap.end()) {
        return {};
    }
    // Spectators don't currently advertise a relay address — Phase C / Fase 5.
    return {};
}

// ============================================================================
// Outbox (game thread pushes, network thread pops)
// ============================================================================

void SpectatorManager::enqueueOut(OutPacket pkt) {
    // Caller already holds _outMutex (we're inside promotePending /
    // frameStepSpectators / newRngState / step). So no lock here.
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
