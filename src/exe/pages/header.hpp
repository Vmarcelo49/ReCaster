// src/exe/pages/header.hpp
//
// Top navigation bar of the launcher. Fixed 1024×72 strip with:
//   - Left: 3 nav-tabs (PLAY / CONFIG / CONTROLLER) with underline indicator
//   - Right: brand block ("NETPLAY CLIENT" kicker + "RE CASTER" name)
//
// The nav-tabs replace the old left sidebar (P/C/M buttons). Clicking a
// tab sets the active page. There is no longer a Quit button — the user
// closes the window via the OS window-close button.

#pragma once

#include "../ui_state.hpp"

namespace caster::exe::pages::header {

// Draw the header bar at (0, 0). Updates `current_page` when a nav-tab is
// clicked. Must be called inside an ImGui::Begin/End block.
void draw(MenuPage& current_page);

} // namespace caster::exe::pages::header
