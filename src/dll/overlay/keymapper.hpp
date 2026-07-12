// src/dll/overlay/keymapper.hpp
//
// In-game controller mapping overlay. Ported from CCCaster's
// DllControllerManager::handleMappingOverlay(), adapted to ReCaster's
// structure (uses our ControllerMapping + binder + overlay_ui).
//
// Triggered by the '4' top-row number key (polled via GetAsyncKeyState in
// dll_main.cpp's callback()). Saves the result to caster/mapping.ini.
//
// UX (buttons only — directions must be configured via the launcher GUI):
//   1. User presses '4' → overlay activates. Devices are pre-assigned to
//      their current mapping.ini positions (P1 left, P2 right, unassigned
//      in the center).
//   2. Left/Right moves a device between center/P1/P2 (3-state):
//        Left moves leftward:  P2 → center → P1 (stay on further Left)
//        Right moves rightward: P1 → center → P2 (stay on further Right)
//   3. Once assigned, the overlay shows a list of 9 button actions
//      (A/B/C/D/E/AB/Start/FN1/FN2) with current bindings. Navigate with
//      Up/Down (respects the player's mapped direction keys, e.g. WASD).
//      Press Enter (keyboard) or any button (joystick) on a button row to
//      capture a new binding.
//   4. "Done" marks a player as finished. When BOTH players are done, the
//      keymapper saves mapping.ini and returns to gameplay. Toggling off
//      via '4' discards unsaved changes.
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
// captures bindings when in mapping mode. Called once per frame from
// callback() while the keymapper is active; replaces the regular
// frameStep + overlay::updateText path.
//
//   joys[2]       — open SDL_Joystick handles for P1/P2 (nullptr if device
//                   is keyboard or not connected)
//   mappings[2]   — current controller mappings (read/write; written back
//                   to mapping.ini on save)
//   mappingPath   — path to caster/mapping.ini for persistence
void update(std::array<SDL_Joystick*, 2> joys,
            std::array<caster::common::controller::ControllerMapping*, 2> mappings,
            const std::string& mappingPath);

// Keyboard event hook. Called from both the WindowProcHook (lifecycle.cpp)
// and the hotkey polling (dll_main.cpp) when the mapper is active. Returns
// true if the event was consumed (should not propagate to the game), false
// otherwise.
//
//   vkCode   — Win32 virtual-key code
//   isDown   — true = keydown, false = keyup
bool handleKeyEvent(uint32_t vkCode, bool isDown);

} // namespace caster::dll::overlay::keymapper
