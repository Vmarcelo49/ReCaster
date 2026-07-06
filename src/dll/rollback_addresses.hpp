// src/dll/rollback_addresses.hpp
//
// Builds the MemDumpList of all memory addresses that the RollbackManager
// needs to save/restore. This replaces the CCCaster's Generator.cpp +
// rollback.bin binary blob — we build the list at runtime instead.
//
// All addresses are for MBAA.exe 1.07 Rev.1.4.0 (32-bit).

#pragma once

#include "mem_dump.hpp"
#include "constants.hpp"

#include <cstdint>

namespace caster::dll {

// Additional constants from Generator.cpp (not in constants.hpp)
inline constexpr std::uintptr_t CC_P1_EXTRA_STRUCT_ADDR     = 0x557DB8;
inline constexpr std::uintptr_t CC_P2_EXTRA_STRUCT_ADDR     = 0x557FC4;
inline constexpr std::size_t    CC_EXTRA_STRUCT_SIZE         = 0x20C;

inline constexpr std::uintptr_t CC_P1_SPELL_CIRCLE_ADDR     = 0x5641A4;
inline constexpr std::uintptr_t CC_P2_SPELL_CIRCLE_ADDR     = 0x564200;

inline constexpr std::uintptr_t CC_METER_ANIMATION_ADDR     = 0x7717D8;

inline constexpr std::uintptr_t CC_EFFECTS_ARRAY_ADDR       = 0x67BDE8;
inline constexpr int            CC_EFFECTS_ARRAY_COUNT      = 1000;
inline constexpr std::size_t    CC_EFFECT_ELEMENT_SIZE      = 0x33C;

inline constexpr std::uintptr_t CC_SUPER_FLASH_PAUSE_ADDR   = 0x5595B4;
inline constexpr std::uintptr_t CC_SUPER_FLASH_TIMER_ADDR   = 0x562A48;

inline constexpr std::uintptr_t CC_SUPER_STATE_ARRAY_ADDR   = 0x558608;
inline constexpr std::size_t    CC_SUPER_STATE_ARRAY_SIZE   = 5 * 0x30C;

inline constexpr std::uintptr_t CC_P1_STATUS_MSG_ARRAY_ADDR = 0x563580;
inline constexpr std::uintptr_t CC_P2_STATUS_MSG_ARRAY_ADDR = 0x5635F4;
inline constexpr std::size_t    CC_STATUS_MSG_ARRAY_SIZE    = 0x60;

inline constexpr std::uintptr_t CC_CAMERA_SCALE_1_ADDR      = 0x54EB70;
inline constexpr std::uintptr_t CC_CAMERA_SCALE_2_ADDR      = 0x54EB74;
inline constexpr std::uintptr_t CC_CAMERA_SCALE_3_ADDR      = 0x54EB78;

inline constexpr std::uintptr_t CC_INPUT_STATE_ADDR         = 0x562A6F;
inline constexpr std::uintptr_t CC_SLOW_TIMER_INIT_ADDR     = 0x562A6C;
inline constexpr std::uintptr_t CC_SLOW_TIMER_ADDR          = 0x55D208;

inline constexpr std::uintptr_t CC_GRAPHICS_ARRAY_ADDR      = 0x61E170;
inline constexpr std::size_t    CC_GRAPHICS_ARRAY_SIZE      = 4000 * 0x60;

inline constexpr std::uintptr_t CC_GRAPHICS_COUNTER         = 0x67BD78;

// Build and return the complete MemDumpList for rollback.
// This is called once by RollbackManager::allocateStates().
inline MemDumpList buildRollbackAddresses() {
    MemDumpList allAddrs;

    // ---- Misc addresses ----
    // Game state
    allAddrs.append({(void*)CC_ROUND_TIMER_ADDR, 4});
    allAddrs.append({(void*)CC_REAL_TIMER_ADDR, 4});
    allAddrs.append({(void*)CC_WORLD_TIMER_ADDR, 4});
    allAddrs.append({(void*)CC_SLOW_TIMER_INIT_ADDR, 2});
    allAddrs.append({(void*)CC_SLOW_TIMER_ADDR, 2});
    allAddrs.append({(void*)CC_INTRO_STATE_ADDR, 1});
    allAddrs.append({(void*)CC_INPUT_STATE_ADDR, 1});
    allAddrs.append({(void*)CC_SKIPPABLE_FLAG_ADDR, 4});

    // RNG state
    allAddrs.append({(void*)CC_RNG_STATE0_ADDR, 4});
    allAddrs.append({(void*)CC_RNG_STATE1_ADDR, 4});
    allAddrs.append({(void*)CC_RNG_STATE2_ADDR, 4});
    allAddrs.append({(void*)CC_RNG_STATE3_ADDR, (std::size_t)CC_RNG_STATE3_SIZE});

    // Unknown states
    allAddrs.append({(void*)0x563864, 4});
    allAddrs.append({(void*)0x56414C, 4});

    // Graphical effects
    allAddrs.append({(void*)CC_GRAPHICS_ARRAY_ADDR, CC_GRAPHICS_ARRAY_SIZE});
    allAddrs.append({(void*)CC_GRAPHICS_COUNTER, 4});

    allAddrs.append({(void*)CC_SUPER_FLASH_PAUSE_ADDR, 4});
    allAddrs.append({(void*)CC_SUPER_FLASH_TIMER_ADDR, 4});
    allAddrs.append({(void*)CC_SUPER_STATE_ARRAY_ADDR, CC_SUPER_STATE_ARRAY_SIZE});

    // Player extra structs
    allAddrs.append({(void*)CC_P1_EXTRA_STRUCT_ADDR, CC_EXTRA_STRUCT_SIZE});
    allAddrs.append({(void*)CC_P2_EXTRA_STRUCT_ADDR, CC_EXTRA_STRUCT_SIZE});

    // Wins
    allAddrs.append({(void*)CC_P1_WINS_ADDR, 4});
    allAddrs.append({(void*)CC_P2_WINS_ADDR, 4});
    allAddrs.append({(void*)CC_P1_GAME_POINT_FLAG_ADDR, 4});
    allAddrs.append({(void*)CC_P2_GAME_POINT_FLAG_ADDR, 4});

    // HUD
    allAddrs.append({(void*)CC_METER_ANIMATION_ADDR, 4});
    allAddrs.append({(void*)CC_P1_SPELL_CIRCLE_ADDR, 4});
    allAddrs.append({(void*)CC_P2_SPELL_CIRCLE_ADDR, 4});
    allAddrs.append({(void*)CC_P1_STATUS_MSG_ARRAY_ADDR, CC_STATUS_MSG_ARRAY_SIZE});
    allAddrs.append({(void*)CC_P2_STATUS_MSG_ARRAY_ADDR, CC_STATUS_MSG_ARRAY_SIZE});

    // Intro/outro graphics
    allAddrs.append({(void*)0x74D9D0, 4});
    allAddrs.append({(void*)0x74E4E4, 4});
    allAddrs.append({(void*)0x74E4E8, 4});
    allAddrs.append({(void*)0x74D598, 4});
    allAddrs.append({(void*)0x74E5B0, 4});
    allAddrs.append({(void*)0x74E768, 4});
    allAddrs.append({(void*)0x74E770, 0x74E784 - 0x74E770});
    allAddrs.append({(void*)0x74E78C, 0x74E798 - 0x74E78C});
    allAddrs.append({(void*)0x74E79C, 0x74E7A8 - 0x74E79C});
    allAddrs.append({(void*)0x74E7AC, 0x74E7C0 - 0x74E7AC});
    allAddrs.append({(void*)0x74E7C8, 0x74E7D8 - 0x74E7C8});
    allAddrs.append({(void*)0x74E7DC, 0x74E7E0 - 0x74E7DC});
    allAddrs.append({(void*)0x74E7E4, 0x74E7F4 - 0x74E7E4});
    allAddrs.append({(void*)0x74E7F8, 0x74E808 - 0x74E7F8});
    allAddrs.append({(void*)0x74E80C, 0x74E810 - 0x74E80C});
    allAddrs.append({(void*)0x74E814, 0x74E828 - 0x74E814});
    allAddrs.append({(void*)0x74E82C, 0x74E834 - 0x74E82C});
    allAddrs.append({(void*)0x74E838, 0x74E84C - 0x74E838});
    allAddrs.append({(void*)0x74E850, 0x74E858 - 0x74E850});
    allAddrs.append({(void*)0x74E85C, 0x74E86C - 0x74E85C});
    allAddrs.append({(void*)0x76E780, 0x76E78C - 0x76E780});

    // Camera position
    allAddrs.append({(void*)0x555124, 4});
    allAddrs.append({(void*)0x555128, 4});
    allAddrs.append({(void*)0x5585E8, 0x5585F4 - 0x5585E8});
    allAddrs.append({(void*)0x55DEC4, 0x55DED0 - 0x55DEC4});
    allAddrs.append({(void*)0x55DEDC, 0x55DEE8 - 0x55DEDC});
    allAddrs.append({(void*)0x564B14, 0x564B20 - 0x564B14});
    allAddrs.append({(void*)0x564B10, 2});
    allAddrs.append({(void*)0x563750, 4});
    allAddrs.append({(void*)0x557DB0, 4});
    allAddrs.append({(void*)0x557DB4, 4});
    allAddrs.append({(void*)0x557D2B, 1});
    allAddrs.append({(void*)0x557DAC, 2});
    allAddrs.append({(void*)0x559546, 2});
    allAddrs.append({(void*)0x564B00, 2});
    allAddrs.append({(void*)0x76E6F8, 4});
    allAddrs.append({(void*)0x76E6FC, 4});
    allAddrs.append({(void*)0x7B1D2C, 4});

    // Camera scaling
    allAddrs.append({(void*)0x55D204, 4});
    allAddrs.append({(void*)0x56357C, 4});
    allAddrs.append({(void*)0x55DEE8, 4});
    allAddrs.append({(void*)0x564B0C, 4});
    allAddrs.append({(void*)0x564AF8, 4});
    allAddrs.append({(void*)0x564B24, 4});
    allAddrs.append({(void*)0x76E6F4, 4});
    allAddrs.append({(void*)CC_CAMERA_SCALE_1_ADDR, 4});
    allAddrs.append({(void*)CC_CAMERA_SCALE_2_ADDR, 4});
    allAddrs.append({(void*)CC_CAMERA_SCALE_3_ADDR, 4});

    // ---- Player struct addresses (P1, P2, P3, P4) ----
    // From Generator.cpp playerAddrs vector. Each player gets the same set
    // offset by CC_PLR_STRUCT_SIZE.
    const std::vector<std::pair<uint32_t, uint32_t>> playerRanges = {
        {0x555130, 0x555140}, {0x555140, 0x555160}, {0x555160, 0x555180},
        {0x555180, 0x555188}, {0x555188, 0x555190}, {0x555190, 0x555240},
        {0x555240, 0x555244}, {0x555244, 0x555284}, {0x555284, 0x555288},
        {0x555288, 0x5552EC}, {0x5552EC, 0x5552F0}, {0x5552F0, 0x5552F4},
        {0x5552F4, 0x555310}, {0x555310, 0x55532C}, {0x55532C, 0x555330},
        {0x555330, 0x55534C}, {0x55534C, 0x55535C}, {0x55535C, 0x5553CC},
        {0x5553CC, 0x5553D0}, {0x5553D0, 0x5553EC}, {0x5553EC, 0x5553F0},
        {0x5553F0, 0x5553F4}, {0x5553F4, 0x5553FC}, {0x5553FC, 0x555400},
        {0x555400, 0x555404}, {0x555404, 0x555410}, {0x555410, 0x55542C},
        {0x55542C, 0x555430}, {0x555430, 0x55544C}, {0x55544C, 0x555450},
        {0x555450, 0x555454}, {0x555454, 0x555458}, {0x555458, 0x55545C},
        {0x55545C, 0x555460}, {0x555460, 0x555464}, {0x555464, 0x55546C},
        {0x55546C, 0x555470}, {0x555470, 0x55550C}, {0x55550C, 0x555510},
        {0x555510, 0x555518}, {0x555518, 0x55561A}, {0x55561A, 0x55571C},
        {0x55571C, 0x55581E}, {0x55581E, 0x555920}, {0x555920, 0x555A22},
        {0x555A22, 0x555B24}, {0x555B24, 0x555B2C}, {0x555B2C, 0x555C2C},
    };

    for (int p = 0; p < 4; ++p) {
        uint32_t offset = p * CC_PLR_STRUCT_SIZE;
        for (const auto& [start, end] : playerRanges) {
            allAddrs.append({(void*)(uintptr_t)(start + offset), end - start});
        }
    }

    // ---- Effects array (1000 elements × 0x33C bytes each) ----
    // Each effect has a pointer at offset 0x320+0x38 that points to a
    // sub-struct in heap-allocated memory. The sub-struct has another
    // pointer at offset 0, which points to yet another struct (4 bytes).
    //
    // This matches CCCaster's `firstEffect` MemDump from Generator.cpp:291:
    //   MemDumpPtr(0x320, 0x38, 4, {
    //       MemDumpPtr(0, 0, 4, {
    //           MemDumpPtr(0, 0, 4)
    //       })
    //   })
    //
    // Without this pointer chasing, the rollback only restores the
    // direct 0x33C bytes of each effect but loses the heap-allocated
    // sub-state (animation frames, texture refs, etc.), causing visual
    // desyncs that compound over time.
    for (int i = 0; i < CC_EFFECTS_ARRAY_COUNT; ++i) {
        const uintptr_t base = CC_EFFECTS_ARRAY_ADDR + i * CC_EFFECT_ELEMENT_SIZE;
        MemDump effectDump(
            (void*)base,
            CC_EFFECT_ELEMENT_SIZE,
            {
                MemDumpPtr(0x320, 0x38, 4, {
                    MemDumpPtr(0, 0, 4, {
                        MemDumpPtr(0, 0, 4)
                    })
                })
            }
        );
        allAddrs.append(effectDump);
    }

    allAddrs.update();
    return allAddrs;
}

} // namespace caster::dll
