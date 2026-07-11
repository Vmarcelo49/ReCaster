// src/dll/input/input_reader.hpp
//
// Reads local controller input every frame and converts to the MBAA
// numpad+buttons format. Reuses the same ControllerMapping that the GUI
// configures (mapping.ini is shared between launcher and DLL).
//
// This replaces CCCaster's DllControllerManager + DllControllerUtils
// (~1260 LOC) with ~200 LOC by leveraging our existing mapping.hpp + binder
// logic + SDL2 directly.

#pragma once

#include "../common/controller/mapping.hpp"

#include <cstdint>

struct _SDL_Joystick;
using SDL_Joystick = struct _SDL_Joystick;

namespace caster::dll {

// Read the local player's input and return it in MBAA numpad+buttons format.
//
//   joy      — open SDL_Joystick handle (nullptr for keyboard)
//   mapping  — the player's ControllerMapping (loaded from mapping.ini)
//
// Returns a uint16_t where:
//   bits 0-3: direction in numpad notation (0=neutral, 1=DL, 2=D, 3=DR,
//             4=L, 6=R, 7=UL, 8=U, 9=UR)
//   bits 4+:  button bitmask (CC_BUTTON_A, CC_BUTTON_B, etc.)
//
// The direction and buttons are combined as: (direction) | (buttons << 4)
// ... actually, looking at the CCCaster code more carefully, the game
// expects direction in the low nibble and buttons in the upper bits,
// combined via the COMBINE_INPUT macro: (direction) | (buttons << 4).
// But actually the game stores direction and buttons as separate fields,
// so we return them packed and the caller (writeGameInput) unpacks.
//
// Actually, looking at DllProcessManager::writeGameInput, it takes
// separate direction and buttons params. So we return a struct.

struct GameInput {
    uint16_t direction = 0;  // numpad notation (0=neutral, 5→0)
    uint16_t buttons   = 0;  // CC_BUTTON_* bitmask
};

// Read local input. `joy` is the open SDL_Joystick (nullptr if keyboard).
// `mapping` is the player's controller mapping.
GameInput read_local_input(SDL_Joystick* joy,
                           const caster::common::controller::ControllerMapping& mapping);

// Convert a GameInput to the combined uint16_t format used by the
// InputsContainer (direction in low nibble, buttons in high bits).
// This matches the CCCaster COMBINE_INPUT macro:
//   (direction & 0xF) | (buttons << 4)
inline uint16_t combine_input(const GameInput& gi) {
    return (gi.direction & 0x0F) | (gi.buttons << 4);
}

// SOCD filter: resolve simultaneous opposing directions.
// Returns the filtered direction state (as up/down/left/right bools).
struct DirState {
    bool up = false, down = false, left = false, right = false;
};

DirState filter_socd(const DirState& raw,
                     caster::common::controller::SocdMode mode);

// ---- Native MBAA keyboard reader ----
//
// Reads keyboard input directly from MBAA.exe's own keyboard config (the
// 10-byte scan-code table at offset 0x14D2C0 in MBAA.exe). This is the
// same approach zzcaster uses (keyboard.zig) and works independently of
// the caster mapping.ini — it uses whatever keys the player configured
// in MBAA.exe's own config dialog (which we skip via ASM patches).
//
// Returns a GameInput with direction + buttons in the same format as
// read_local_input. If the keyboard config can't be read (wrong exe
// version, file not found), returns a zero GameInput.
struct GameInput read_native_keyboard();

} // namespace caster::dll
