// src/exe/pages/controller_helpers.hpp
//
// UI helpers for the controllers page. Wraps the model (ControllerMapping)
// and the binder (poll_for_bind_input) in ImGui drawing routines.
//
// Layout and widths are ported 1:1 from zzcaster's `ui_controller_mapper.zig`
// so the visual identity matches.

#pragma once

#include "../../common/controller/mapping.hpp"

#include <cstdint>
#include <string>
#include <vector>

// Forward decls — we don't want SDL2/ImGui headers in this header if we
// can avoid it, but bindButton needs BindingTarget/ControllerMapping
// and we want SDL_Joystick* for the open/close lifecycle.
// SDL2 declares `typedef struct _SDL_Joystick SDL_Joystick;`.
struct _SDL_Joystick;
using SDL_Joystick = struct _SDL_Joystick;

namespace caster::exe::pages::controller_helpers {

// One entry in the device list (Keyboard + up to 15 joysticks).
struct DeviceEntry {
    std::string display_name;   // e.g. "Xbox 360 Controller"
    int         device_index;   // -1 = Keyboard, 0+ = SDL joystick index
};

// Build the list of currently-available input devices. Always includes
// "Keyboard" as the first entry. Returns up to 16 entries.
std::vector<DeviceEntry> build_device_list();

// Draw a single bind button. The button label shows the action name and
// the current binding (or "Press..." if this target is currently being
// bound). Width is fixed at 90px.
//
// Returns true if the user clicked the button (caller then enters bind
// mode by setting bind_target = target and starting a cooldown).
//
//   label          — short action name ("A", "Up", "FN1", etc.)
//   target         — which binding slot this button corresponds to
//   current        — the current InputBinding for this slot
//   bind_target    — IN/OUT: which slot is currently in bind mode
//                    (BindingTarget::None = idle)
//   cooldown_until_ms — IN/OUT: wall-clock ms timestamp; bind mode is
//                    suppressed while now < cooldown_until_ms
//   now_ms         — current wall-clock time in ms
bool bind_button(const char* label,
                 caster::common::controller::BindingTarget target,
                 const caster::common::controller::InputBinding& current,
                 caster::common::controller::BindingTarget& bind_target,
                 std::int64_t& cooldown_until_ms,
                 std::int64_t now_ms);

// List view: 13 rows + SOCD + macro + deadzone + actions.
// Returns true if any change was made (caller does autosave).
//
//   name           — player label ("Player 1" / "Player 2")
//   mapping        — IN/OUT: the player's ControllerMapping
//   bind_target    — IN/OUT: which slot is in bind mode
//   joy            — IN/OUT: currently-open SDL_Joystick (closed + reopened
//                    when device changes)
//   device_sel     — IN/OUT: index into the device combo (0 = Keyboard,
//                    1+ = joystick N). Differs from mapping.device_index
//                    by 1 (Keyboard is at combo index 0 but device_index -1).
//   cooldown_until_ms — IN/OUT: bind cooldown timestamp
//   now_ms         — current wall-clock ms
bool draw_list_panel(const char* name,
                     caster::common::controller::ControllerMapping& mapping,
                     caster::common::controller::BindingTarget& bind_target,
                     SDL_Joystick*& joy,
                     int& device_sel,
                     std::int64_t& cooldown_until_ms,
                     std::int64_t now_ms);

} // namespace caster::exe::pages::controller_helpers
