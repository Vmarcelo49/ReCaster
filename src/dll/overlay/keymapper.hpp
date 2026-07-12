// src/dll/overlay/keymapper.hpp
//
// In-game controller mapping overlay. Ported from CCCaster's
// DllControllerManager::handleMappingOverlay() (~270 LOC), adapted to
// ReCaster's structure (uses our ControllerMapping + binder + overlay_ui).
//
// Triggered by the '4' top-row number key (WindowProcHook in lifecycle.cpp).
// Saves the result to caster/mapping.ini (same file the launcher uses).
//
// UX (mirrors CCCaster):
//   1. User presses '4' → overlay activates in "select player" mode.
//      Left column says "Press Left on P1 controller".
//      Right column says "Press Right on P2 controller".
//   2. When the user presses Left/Right on any connected controller (or
//      arrow keys on keyboard), that device is assigned to that player.
//   3. The overlay switches to "mapping" mode: shows a list of 13 actions
//      (Up/Down/Left/Right/A/B/C/D/E/AB/Start/FN1/FN2) with their current
//      bindings. User navigates with Up/Down on the assigned device,
//      presses Left (P1) or Right (P2) to delete a binding,
//      presses Enter (keyboard) or any button (joystick) to capture a
//      new binding.
//   4. Selecting "Done" (last option) closes the mapper for that player.
//      When both players are done (or unassigned), the overlay closes and
//      mapping.ini is saved.
//
// Concurrent mapping: both players can map at the same time (one device
// each), exactly like CCCaster.

#pragma once

#include <array>
#include <cstdint>
#include <string>

struct _SDL_Joystick;
using SDL_Joystick = struct _SDL_Joystick;

namespace caster::common::controller {
    struct ControllerMapping;
}

namespace caster::dll::overlay::keymapper {

// Activate / deactivate the mapper overlay. Called from the '4' hotkey.
// toggle() on an active mapper closes it (discards unsaved changes).
void toggle();

// True while the mapper overlay is active (drives the per-frame update
// in dll_main.cpp's callback).
bool isActive();

// Per-frame update. Reads controller state, updates the overlay text,
// captures bindings when in mapping mode. Called from callback() after
// frameStep() and before overlay::updateText().
//
//   joys[2]       — open SDL_Joystick handles for P1/P2 (nullptr if device
//                   is keyboard or not connected)
//   mappings[2]   — current controller mappings (read/write; written back
//                   to mapping.ini on save)
//   mappingPath   — path to caster/mapping.ini for persistence
void update(std::array<SDL_Joystick*, 2> joys,
            std::array<caster::common::controller::ControllerMapping*, 2> mappings,
            const std::string& mappingPath);

// Keyboard event hook (called from WindowProcHook when the mapper is
// active). Returns true if the event was consumed (should not propagate
// to the game), false otherwise.
//
//   vkCode   — Win32 virtual-key code
//   isDown   — true = keydown, false = keyup
bool handleKeyEvent(uint32_t vkCode, bool isDown);

} // namespace caster::dll::overlay::keymapper
