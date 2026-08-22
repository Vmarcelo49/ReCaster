// src/dll/spec/spectate_client.cpp
//
// Phase C / Fase 3 — Client-side spectator support (implementation).
//
// See spectate_client.hpp for the design.

#include "spectate_client.hpp"
#include "../netplay/manager.hpp"
#include "../common/logger.hpp"

namespace caster::dll::spec {

SpectateClient::SpectateClient(NetplayManager* netManPtr)
    : _netManPtr(netManPtr)
{
}

// Out-of-line destructor — see header for rationale.
SpectateClient::~SpectateClient() = default;

// ============================================================================
// Message handlers
// ============================================================================

void SpectateClient::onSpectateConfig(const SpectateConfig& sc) {
    if (configReceived_) {
        common::logger::warn("spectate_client: duplicate SpectateConfig — ignoring");
        return;
    }
    if (!_netManPtr) return;

    common::logger::info("spectate_client: SpectateConfig received — "
                         "delay={} rollback={} rollbackDelay={} winCount={} "
                         "hostPlayer={} isTraining={} name0='{}' name1='{}'",
                         sc.delay, sc.rollback, sc.rollbackDelay, sc.winCount,
                         sc.hostPlayer, sc.isTraining, sc.names[0], sc.names[1]);

    // Configure the NetplayManager as a spectator.
    auto& nc = _netManPtr->config;
    nc.mode.value = ClientMode::Mode::SpectateNetplay;
    nc.mode.flags = sc.isTraining ? ClientMode::Training : 0;
    nc.delay = sc.delay;
    nc.rollback = sc.rollback;
    nc.rollbackDelay = sc.rollbackDelay;
    nc.winCount = sc.winCount;
    nc.hostPlayer = sc.hostPlayer;
    nc.names = sc.names;

    // Spectator is always "player 2" locally — the host is player 1.
    // This matches CCCaster's convention.
    _netManPtr->setRemotePlayer(1);

    configReceived_ = true;
}

void SpectateClient::onInitialGameState(const InitialGameState& igs) {
    if (initialReceived_) {
        common::logger::warn("spectate_client: duplicate InitialGameState — ignoring");
        return;
    }
    if (!_netManPtr) return;

    common::logger::info("spectate_client: InitialGameState received — "
                         "idx={} frm={} state={} stage={} "
                         "chara=({}, {}) moon=({}, {}) color=({}, {})",
                         igs.indexedFrame.parts.index, igs.indexedFrame.parts.frame,
                         igs.netplayState, igs.stage,
                         igs.chara[0], igs.chara[1],
                         igs.moon[0], igs.moon[1],
                         igs.color[0], igs.color[1]);

    // Write the chara/moon/color/stage into game memory so the Loading
    // transition picks them up. The igs already has these fields populated
    // by the host; we just need to copy them to the game's select memory.
    *asU32(CC_STAGE_SELECTOR_ADDR) = igs.stage;
    *asU32(CC_P1_CHARACTER_ADDR)   = igs.chara[0];
    *asU32(CC_P2_CHARACTER_ADDR)   = igs.chara[1];
    *asU32(CC_P1_MOON_SELECTOR_ADDR) = igs.moon[0];
    *asU32(CC_P2_MOON_SELECTOR_ADDR) = igs.moon[1];
    *asU32(CC_P1_COLOR_SELECTOR_ADDR) = igs.color[0];
    *asU32(CC_P2_COLOR_SELECTOR_ADDR) = igs.color[1];

    // Store the InitialGameState on the NetplayManager — needed by
    // getAutoCharaSelectInput to know what to write each frame.
    _netManPtr->initial = igs;

    // Force the FSM into AutoCharaSelect (spectators skip CharaSelect).
    // The transition PreInitial → Initial → AutoCharaSelect is the
    // standard spectator path (see states.hpp).
    if (_netManPtr->getState() == NetplayState::PreInitial) {
        _netManPtr->setState(NetplayState::Initial);
    }
    if (_netManPtr->getState() == NetplayState::Initial) {
        _netManPtr->setState(NetplayState::AutoCharaSelect);
    }

    currentPosition_ = igs.indexedFrame;
    initialReceived_ = true;

    // Reset stream-gap tracking — the BothInputs history restarts at
    // this position.
    lastBatchIndex_ = igs.indexedFrame.parts.index;
    nextExpectedStart_ = igs.indexedFrame.parts.frame;
}

void SpectateClient::onRngState(const RngState& rs) {
    if (!_netManPtr) return;

    // Issue #5: DEFER — buffer by index. The host sends each round's
    // RngState when its SEND position crosses that index, which can be
    // many seconds before our playback reaches it (and at promotion time
    // it arrives while we're still booting through menus). Applying
    // instantly would clobber the RNG of the round being replayed and
    // permanently desync the visuals.
    _pendingRng[rs.index] = rs;
    applyPendingRng();
}

void SpectateClient::applyPendingRng() {
    if (_pendingRng.empty()) return;
    const uint32_t playedIndex = currentPosition_.parts.index;
    for (auto it = _pendingRng.begin(); it != _pendingRng.end();) {
        if (it->first > playedIndex) break;  // map is ordered by index
        common::logger::info(
            "spectate_client: applying deferred RngState for idx={} "
            "(playback at idx={})", it->first, playedIndex);
        _netManPtr->setRngState(it->second);
        it = _pendingRng.erase(it);
    }
}

void SpectateClient::onMenuIndex(const MenuIndex& mi) {
    if (!_netManPtr) return;

    // Issue #5: DEFER — same contract as RngState. The archive dump
    // delivers every round's menu choice up-front; writing them instantly
    // would populate retry-menu slots for rounds the spectator hasn't
    // reached, driving a premature rematch.
    _pendingMenu[mi.index] = mi;
    applyPendingMenu();
}

void SpectateClient::applyPendingMenu() {
    if (_pendingMenu.empty()) return;
    const uint32_t playedIndex = currentPosition_.parts.index;
    for (auto it = _pendingMenu.begin(); it != _pendingMenu.end();) {
        if (it->first > playedIndex) break;
        common::logger::info(
            "spectate_client: applying deferred MenuIndex for idx={} (choice={})",
            it->first, it->second.menuIndex);
        _netManPtr->setRetryMenuIndex(it->first, it->second.menuIndex);
        it = _pendingMenu.erase(it);
    }
}

void SpectateClient::syncToStreamHead() {
    if (!_netManPtr) return;
    const uint32_t target = _receivedHead.parts.index;
    if (target <= _netManPtr->getIndex()) return;

    // Drop buffered data from BEFORE the jump point — those frames are
    // gone on the host side too (world-timer freeze / index rollover).
    for (auto it = _futureBatches.begin(); it != _futureBatches.end();) {
        if (it->first < target) it = _futureBatches.erase(it); else ++it;
    }
    for (auto it = _pendingRng.begin(); it != _pendingRng.end();) {
        if (it->first < target) it = _pendingRng.erase(it); else ++it;
    }
    for (auto it = _pendingMenu.begin(); it != _pendingMenu.end();) {
        if (it->first < target) it = _pendingMenu.erase(it); else ++it;
    }

    common::logger::info(
        "spectate_client: boundary jump → idx={}", target);
    _netManPtr->jumpPlaybackToIndex(target);

    currentPosition_.parts.index = target;
    currentPosition_.parts.frame = 0;
    lastBatchIndex_ = target;
    nextExpectedStart_ = 0;
    jumpResync_ = true;   // first post-jump batch defines the baseline
}

void SpectateClient::onBothInputs(const BothInputs& bi) {
    if (!_netManPtr) return;

    _receivedHead = bi.indexedFrame;

    // Issue #5 archive replay: batches for FUTURE indices arrive long
    // before playback reaches them (the promotion dump races ahead).
    // Buffer by index; flushReadyBatches() feeds the NetplayManager in
    // order as playback advances.
    _futureBatches[bi.getIndex()].push_back(bi);
    flushReadyBatches();
}

void SpectateClient::flushReadyBatches() {
    bool applied_any = false;

    for (;;) {
        auto it = _futureBatches.begin();
        if (it == _futureBatches.end()) break;
        if (it->first > currentPosition_.parts.index + 1) break;  // still future

        for (const BothInputs& bi : it->second) {
            applyOneBatch(bi);
        }
        _futureBatches.erase(it);
        applied_any = true;
    }

    if (applied_any) {
        applyPendingRng();
        applyPendingMenu();
    }
}

void SpectateClient::applyOneBatch(const BothInputs& bi) {
    // Gap detection (issue #5 diagnostics): within a transition index,
    // batches must cover [startFrm .. startFrm+size) contiguously. The
    // cross-index TAIL batch intentionally OVERLAPS the next index's
    // head (manager sends old-index tail while cursor jumps ahead), so
    // overlap is accepted — only forward holes warn.
    {
        const uint32_t start = bi.getStartFrame();
        const uint32_t end   = start + static_cast<uint32_t>(bi.size());
        if (jumpResync_) {
            // First batch after a boundary jump defines the new baseline
            // — pre-jump expectations don't apply.
            jumpResync_ = false;
        } else if (bi.getIndex() == lastBatchIndex_ &&
                   start > nextExpectedStart_) {
            common::logger::warn(
                "spectate_client: SPEC-GAP idx={} got=[{}..{}) "
                "expected start={} (hole of {} frames)",
                bi.getIndex(), start, end, nextExpectedStart_,
                start - nextExpectedStart_);
        }
        lastBatchIndex_ = static_cast<std::uint32_t>(bi.getIndex());
        nextExpectedStart_ = end;
    }

    // Forward to NetplayManager. setBothInputs writes both players'
    // inputs at the given indexedFrame. (No per-batch logging here —
    // this runs on the game thread at up to ~2900 batches/s during
    // archive catch-up; use SPEC-GAP warnings for diagnosis.)
    _netManPtr->setBothInputs(bi);

    // Track our position (the indexedFrame of the last applied batch).
    currentPosition_ = bi.indexedFrame;

    // Playback just reached this batch's transition — flush any control
    // messages (RngState / MenuIndex) buffered for it now, so they are
    // in place before the game simulates these frames.
    applyPendingRng();
    applyPendingMenu();
}

} // namespace caster::dll::spec
