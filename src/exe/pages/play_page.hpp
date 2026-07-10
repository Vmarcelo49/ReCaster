// src/exe/pages/play_page.hpp
//
// The "Play" page — netplay + offline launching.
//
// Netplay input field + Host/Join buttons start a NetplaySession and
// transition to WaitingForPeer. Spectate (direct via host:port) is not yet
// implemented — see docs/stubs.md. Training/Versus buttons
// launch the game offline.

#pragma once

#include "../../common/config.hpp"

#include <string>

namespace caster::exe::pages {

class MainMenu;  // forward decl — defined in main_menu.hpp (same namespace)

namespace play_page {

// State held across frames. Owned by MainMenu.
struct State {
    // Unified input field: "Port / IP:Port / #RoomCode"
    char input_buf[128] = {0};

    // Inline message (shown below the input field). Cleared on next action.
    std::string message;
    bool        is_error = false;
};

// Draw the play page. `cfg` is read-only here (netplay uses it but doesn't
// modify it). `menu` is used to trigger state transitions (InGame after
// offline launch, WaitingForPeer after netplay start, ErrorState on failure).
void draw(const caster::common::config::Config& cfg,
          MainMenu* menu,
          State& state);

} // namespace play_page
} // namespace caster::exe::pages
