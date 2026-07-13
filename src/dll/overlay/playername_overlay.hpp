// src/dll/overlay/playername_overlay.hpp
//
// Minimalist player-name overlay for netplay. Shows "P1: <name>" in the
// top-left corner and "P2: <name>" in the top-right corner, each with a
// small semi-transparent black rectangle behind the text for readability.
//
// Unlike the info overlay (overlay_ui), this has no state machine or
// height animation — it's always visible when active, rendered as a
// simple D3D9 Clear() + DrawText() pair every frame.
//
// Activation:
//   - Auto-enables when g_isNetplay becomes true (checked every frame
//     via setNetplayActive()).
//   - User can toggle on/off with the '5' hotkey.
//   - Config: [overlay] playername_enabled (default true),
//             [overlay] playername_position = top|bottom (default top).
//
// Names come from g_netMan.config.names[2] — populated by the ENet
// handshake. Max 16 chars per name (truncated with "…" if longer).

#pragma once

#include <string>

struct IDirect3DDevice9;

namespace caster::dll::overlay::playername {

// Initialize with config values. Called once from lifecycle.cpp after
// the DX9 hook is installed.
//   enabled      — initial visibility (from config.ini)
//   positionTop  — true = top corners, false = bottom corners
void init(bool enabled, bool positionTop);

// Call every frame to update the netplay-active state. When netplay is
// active and the overlay is enabled, it renders. When netplay ends, it
// hides automatically.
void setNetplayActive(bool active);

// Toggle visibility on/off (hotkey '5'). Only works during netplay.
void toggle();

// Set the player names. Called every frame from callback() with the
// latest names from g_netMan.config.names. Empty names are not rendered.
void setNames(const std::string& p1, const std::string& p2);

// Render the overlay. Called from presentFrameBegin() when the DX9
// Present vtable hook fires. Does nothing if not active or names are empty.
void render(IDirect3DDevice9* device);

// True if the overlay is currently visible (active + enabled + netplay).
bool isVisible();

// Release D3D resources (font). Called from InvalidateDeviceObjects()
// when the D3D9 device is lost/reset.
void invalidateDeviceObjects();

} // namespace caster::dll::overlay::playername
