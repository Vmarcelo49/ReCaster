// src/exe/pages/play_page.hpp
//
// The "Play" page — netplay + offline launching.
//
// Phase 5: Training / Versus buttons now actually launch the game via
// GameRunner. Phase 7 will add the netplay input field + Host/Join/Spectate
// buttons that dispatch to the netplay session.

#pragma once

#include "../../common/config.hpp"

namespace caster::exe {

class MainMenu;  // forward decl

namespace pages::play_page {

// Draw the play page inside the content area. The caller has already
// positioned the cursor at the content area's origin and opened a child
// window — we just fill it.
//
// `menu` is used to trigger state transitions (e.g. to InGame after a
// successful launch). Pass nullptr in tests / when you don't care about
// state transitions.
void draw(const caster::common::config::Config& cfg, MainMenu* menu);

} // namespace pages::play_page
} // namespace caster::exe
