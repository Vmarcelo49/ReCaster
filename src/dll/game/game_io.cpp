// src/dll/game/game_io.cpp
// Ported from CCCaster DllProcessManager.cpp.

#include "game_io.hpp"
#include "../common/logger.hpp"

#include <cstring>

namespace caster::dll::process_manager {

void writeGameInput(uint8_t player, uint16_t direction, uint16_t buttons) {
    // Neutral (5) or out-of-range → 0
    if (direction == 5 || direction > 9)
        direction = 0;

    char* baseAddr = *(char**)CC_PTR_TO_WRITE_INPUT_ADDR;
    if (!baseAddr) return;

    switch (player) {
        case 1:
            *(uint16_t*)(baseAddr + CC_P1_OFFSET_DIRECTION) = direction;
            *(uint16_t*)(baseAddr + CC_P1_OFFSET_BUTTONS)   = buttons;
            break;
        case 2:
            *(uint16_t*)(baseAddr + CC_P2_OFFSET_DIRECTION) = direction;
            *(uint16_t*)(baseAddr + CC_P2_OFFSET_BUTTONS)   = buttons;
            break;
    }
}

RngState getRngState(uint32_t index) {
    RngState state(index);
    state.rngState0 = *(uint32_t*)CC_RNG_STATE0_ADDR;
    state.rngState1 = *(uint32_t*)CC_RNG_STATE1_ADDR;
    state.rngState2 = *(uint32_t*)CC_RNG_STATE2_ADDR;
    std::memcpy(state.rngState3.data(), (void*)CC_RNG_STATE3_ADDR, CC_RNG_STATE3_SIZE);
    return state;
}

void setRngState(const RngState& rngState) {
    *(uint32_t*)CC_RNG_STATE0_ADDR = rngState.rngState0;
    *(uint32_t*)CC_RNG_STATE1_ADDR = rngState.rngState1;
    *(uint32_t*)CC_RNG_STATE2_ADDR = rngState.rngState2;
    std::memcpy((void*)CC_RNG_STATE3_ADDR, rngState.rngState3.data(), CC_RNG_STATE3_SIZE);
}

} // namespace caster::dll::process_manager
