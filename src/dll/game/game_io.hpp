// src/dll/game/game_io.hpp
// Ported from CCCaster DllProcessManager.cpp. Writes game input + RNG state.

#pragma once

#include "addresses.hpp"
#include "protocol/messages.hpp"

#include <cstdint>

namespace caster::dll::process_manager {

// Write direction + buttons to the game's input struct for the given player (1 or 2).
void writeGameInput(uint8_t player, uint16_t direction, uint16_t buttons);

// Read the current RNG state from the game.
RngState getRngState(uint32_t index);

// Write an RNG state back to the game (for rollback restore).
void setRngState(const RngState& rngState);

} // namespace caster::dll::process_manager
