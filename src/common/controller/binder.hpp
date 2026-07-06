// src/common/controller/binder.hpp
//
// Polls SDL joystick state (or Win32 keyboard state) to detect the first
// active input. Used by the click-to-bind flow: when the user clicks a
// bind button, the UI calls poll_for_bind_input() each frame until it
// returns a non-empty InputBinding, which is then assigned to the target.
//
// Ported from zzcaster's `controller_mapper.zig::pollForBindInput`.

#pragma once

#include "mapping.hpp"

// Forward decl — we don't want to leak SDL2/SDL_joystick into this header.
// SDL2 declares `typedef struct _SDL_Joystick SDL_Joystick;`, so we forward
// as the underlying struct name to match.
struct _SDL_Joystick;
using SDL_Joystick = struct _SDL_Joystick;

namespace caster::common::controller {

// Returns the first active input detected on the given device, or an
// empty InputBinding (type=None) if nothing is pressed.
//
//   joy          — the open SDL_Joystick handle (or nullptr if device_idx < 0)
//   device_idx   — -1 = keyboard, >=0 = SDL joystick index
//
// Polling priority (matches zzcaster exactly):
//   1. Buttons (first active wins, lowest index first)
//   2. Axes (positive > +20000, then negative < -20000 but > -32000)
//   3. Hats (skips centered; returns packed (dir<<8)|hat_idx)
//   4. Keyboard VK codes 0x08..0xFE (skips mouse 0x01..0x07)
InputBinding poll_for_bind_input(SDL_Joystick* joy, int device_idx);

} // namespace caster::common::controller
