// src/exe/pages/sidebar.hpp
//
// Left sidebar of the launcher: 56px wide, holds the 3 nav buttons
// (P / C / M) at the top and the Quit button (Q) at the bottom.

#pragma once

#include "../ui_state.hpp"

namespace caster::exe::pages::sidebar {

// Draw the sidebar. Updates `current_page` when a nav button is clicked.
// Sets `quit_clicked` to true when the Q button is clicked.
// Must be called inside an ImGui::Begin/End block.
void draw(MenuPage& current_page, bool& quit_clicked);

} // namespace caster::exe::pages::sidebar
