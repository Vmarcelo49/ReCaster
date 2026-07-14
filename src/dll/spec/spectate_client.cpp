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
}

void SpectateClient::onRngState(const RngState& rs) {
    if (!_netManPtr) return;
    _netManPtr->setRngState(rs);
}

void SpectateClient::onMenuIndex(const MenuIndex& mi) {
    if (!_netManPtr) return;
    _netManPtr->setRetryMenuIndex(mi.index, mi.menuIndex);
}

void SpectateClient::onBothInputs(const BothInputs& bi) {
    if (!_netManPtr) return;

    // Forward to NetplayManager. setBothInputs writes both players'
    // inputs at the given indexedFrame.
    _netManPtr->setBothInputs(bi);

    // Track our position (the indexedFrame of the last applied batch).
    currentPosition_ = bi.indexedFrame;
}

} // namespace caster::dll::spec
