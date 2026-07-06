// src/exe/pages/play_page.hpp
//
// The "Play" page — netplay + offline launching.
//
// Phase 7: netplay input field + Host/Join/Spectate buttons are now
// functional UI (they parse input and show inline messages), but the
// actual netplay session start is still a stub that transitions to
// WaitingForPeer (Phase 8 will wire up the real ENet/relay handshake).
// Training/Versus buttons work for real (Phase 5).

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
