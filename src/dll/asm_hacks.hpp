// src/dll/asm_hacks.hpp
// Ported from CCCaster DllAsmHacks.hpp. Stripped: palette, trial, SFX, screenshot.
// Kept: hookMainLoop, hijackControls, hijackMenu, detectRoundStart, multiWindow,
//       hijackEscapeKey, enableDisabledStages, hookPresentCaller, disableFpsLimit.

#pragma once

#include "constants.hpp"
#include "../common/logger.hpp"

#include <cstdint>
#include <vector>
#include <windows.h>

namespace caster::dll::asm_hacks {

// DLL callback function — called every frame by the hooked main loop.
extern "C" void callback();

// Globals updated by ASM hacks
extern uint32_t currentMenuIndex;
extern uint32_t menuConfirmState;
extern uint32_t roundStartCounter;
extern uint8_t  enableEscapeToExit;

// Struct for storing an ASM patch
struct Asm {
    void* const addr;
    const std::vector<uint8_t> bytes;
    mutable std::vector<uint8_t> backup;
    int write() const;
    int revert() const;
};

using AsmList = std::vector<Asm>;

// Macros for inline byte encoding
#define INLINE_DWORD(X) \
    static_cast<unsigned char>(unsigned(X) & 0xFF), \
    static_cast<unsigned char>((unsigned(X) >> 8) & 0xFF), \
    static_cast<unsigned char>((unsigned(X) >> 16) & 0xFF), \
    static_cast<unsigned char>((unsigned(X) >> 24) & 0xFF)

#define INLINE_DWORD_FF { 0xFF, 0x00, 0x00, 0x00 }
#define INLINE_NOP_TWO { 0x90, 0x90 }
#define INLINE_NOP_THREE { 0x90, 0x90, 0x90 }
#define INLINE_NOP_FIVE { 0x90, 0x90, 0x90, 0x90, 0x90 }
#define INLINE_NOP_SIX { 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 }
#define INLINE_NOP_SEVEN { 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 }

#define PATCHJUMP(patchAddr, newAddr) \
    { (void*)(patchAddr), { 0xE9, INLINE_DWORD((unsigned)(newAddr) - (unsigned)(patchAddr) - 5) } }

#define WRITE_ASM_HACK(ASM_HACK) \
    do { const int error = ASM_HACK.write(); \
         if (error != 0) caster::common::logger::err("ASM hack failed: {}", error); \
    } while (0)

// ---- ASM patch lists ----

// Hook the game's main loop to call our callback() every frame
extern const AsmList hookMainLoop;

// Disable normal joystick and keyboard controls (we inject our own)
extern const AsmList hijackControls;

// Copy dynamic menu variables and hijack menu confirms
extern const AsmList hijackMenu;

// Increment round start counter when players can move
extern const AsmList detectRoundStart;

// Enable disabled stages
extern const AsmList enableDisabledStages;

// Skip multiple-instance check
extern const Asm multiWindow;

// Hijack Escape key
extern const Asm hijackEscapeKey;

// Disable FPS limit
extern const Asm disableFpsLimit;

// Disable FPS counter
extern const Asm disableFpsCounter;

// Hook Present caller for frame sync
__attribute__((naked, noinline)) void _naked_presentFuncCaller();
extern const AsmList hookPresentCaller;

// Disable training music reset
extern const Asm disableTrainingMusicReset;

// Fix boss stage super flash overlay
extern const Asm fixBossStageSuperFlashOverlay;

} // namespace caster::dll::asm_hacks
