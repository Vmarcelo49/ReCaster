// src/exe/pages/header.hpp
//
// Header bar of the launcher: fixed 1024×64 strip at the top with the
// "ZZ CASTER" logo on the left and the version string on the right.

#pragma once

namespace caster::exe::pages::header {

// Draw the header bar at (0, 0). Must be called inside an ImGui::Begin/End
// block (uses the window's draw list for the logo).
void draw();

} // namespace caster::exe::pages::header
