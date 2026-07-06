// src/exe/ui_state.hpp
//
// State machine types for the launcher UI. Mirrors zzcaster's
// `UiState` and `MenuPage` enums from `src/launcher/ui.zig`.
//
// UiState controls which FULL-SCREEN view is shown. MenuPage controls which
// page is shown WITHIN the idle state (the main menu's sidebar selection).

#pragma once

namespace caster::exe {

enum class UiState {
    Idle,              // Main menu (Play / Config / Controllers pages)
    WaitingForPeer,    // Netplay handshake in progress
    InGame,            // Game process is running
    ErrorState,        // Fatal error, needs user dismissal
};

enum class MenuPage {
    Play,              // Netplay + Offline cards
    GameConfig,        // Player profile + match rules + network settings
    Controllers,       // P1 + P2 controller mapping
};

} // namespace caster::exe
