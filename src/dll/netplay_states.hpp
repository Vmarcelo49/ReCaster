// src/dll/netplay_states.hpp
//
// Netplay state machine enum. Ported from CCCaster's netplay/NetplayStates.hpp.
//
// State transitions:
//   PreInitial → Initial → { AutoCharaSelect (spectate only), CharaSelect, ReplayMenu (offline only) }
//   { AutoCharaSelect, CharaSelect, ReplayMenu } → Loading
//   Loading → { CharaIntro, InGame (training mode) }
//   CharaIntro → { InGame (versus mode) }
//   Skippable → { InGame (versus mode), RetryMenu }
//   InGame → { Skippable, CharaSelect (not on netplay), ReplayMenu }
//   RetryMenu → { Loading, CharaSelect }

#pragma once

#include <cstdint>
#include <string>

namespace caster::dll {

enum class NetplayState : uint8_t {
    PreInitial      = 0,
    Initial         = 1,
    AutoCharaSelect = 2,
    CharaSelect     = 3,
    Loading         = 4,
    CharaIntro      = 5,
    Skippable       = 6,
    InGame          = 7,
    RetryMenu       = 8,
    ReplayMenu      = 9,
};

inline const char* netplayStateStr(NetplayState state) {
    switch (state) {
        case NetplayState::PreInitial:      return "PreInitial";
        case NetplayState::Initial:         return "Initial";
        case NetplayState::AutoCharaSelect: return "AutoCharaSelect";
        case NetplayState::CharaSelect:     return "CharaSelect";
        case NetplayState::Loading:         return "Loading";
        case NetplayState::CharaIntro:      return "CharaIntro";
        case NetplayState::Skippable:       return "Skippable";
        case NetplayState::InGame:          return "InGame";
        case NetplayState::RetryMenu:       return "RetryMenu";
        case NetplayState::ReplayMenu:      return "ReplayMenu";
        default:                            return "Unknown";
    }
}

// Check if `next` is a valid transition from `current`.
inline bool isValidNextState(NetplayState current, NetplayState next) {
    switch (current) {
        case NetplayState::PreInitial:
            return next == NetplayState::Initial;
        case NetplayState::Initial:
            return next == NetplayState::AutoCharaSelect ||
                   next == NetplayState::CharaSelect ||
                   next == NetplayState::ReplayMenu;
        case NetplayState::AutoCharaSelect:
        case NetplayState::CharaSelect:
        case NetplayState::ReplayMenu:
            return next == NetplayState::Loading;
        case NetplayState::Loading:
            return next == NetplayState::CharaIntro ||
                   next == NetplayState::InGame;
        case NetplayState::CharaIntro:
        case NetplayState::Skippable:
            return next == NetplayState::InGame ||
                   next == NetplayState::RetryMenu ||
                   next == NetplayState::Skippable;
        case NetplayState::InGame:
            return next == NetplayState::Skippable ||
                   next == NetplayState::CharaSelect ||
                   next == NetplayState::ReplayMenu;
        case NetplayState::RetryMenu:
            return next == NetplayState::Loading ||
                   next == NetplayState::CharaSelect;
        default:
            return false;
    }
}

} // namespace caster::dll
