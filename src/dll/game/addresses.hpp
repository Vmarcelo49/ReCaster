// src/dll/game/addresses.hpp
//
// All MBAA.exe 1.07 Rev.1.4.0 memory addresses, game modes, button masks,
// and netplay engine constants. Ported directly from CCCaster's
// netplay/Constants.hpp — every offset is byte-identical.
//
// These addresses are version-specific to MBAACC 1.07 Rev.1.4.0. If the
// game is updated or a different version is used, every CC_*_ADDR must be
// re-verified against the new binary.

#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <utility>

namespace caster::dll {

// Number of frames of inputs to send per message.
inline constexpr int NUM_INPUTS = 30;

// Max allowed rollback frames.
inline constexpr int MAX_ROLLBACK = 15;

// Number of rollback states to allocate.
#ifdef NDEBUG
inline constexpr int NUM_ROLLBACK_STATES = 60;
#else
inline constexpr int NUM_ROLLBACK_STATES = 256;
#endif

// ---- Version / config strings --------------------------------------------

inline constexpr const char* CC_VERSION              = "1.4.0";
inline constexpr const char* CC_TITLE                = "MELTY BLOOD Actress Again Current Code Ver.1.07 Rev.1.4.0";
inline constexpr const char* CC_STARTUP_TITLE        = "MELTY BLOOD Actress Again Current Code Ver.1.07 Rev.1.4.0 ";
inline constexpr const char* CC_STARTUP_BUTTON       = "OK";
inline constexpr const char* CC_NETWORK_CONFIG_FILE  = "System\\NetConnect.dat";
inline constexpr const char* CC_NETWORK_USERNAME_KEY = "UserName";
inline constexpr const char* CC_APP_CONFIG_FILE      = "System\\_App.ini";
inline constexpr const char* CC_APP_WINDOW_MODE_KEY  = "Windowed";

// Location of the keyboard config in the binary.
inline constexpr std::uintptr_t CC_KEYBOARD_CONFIG_OFFSET = 0x14D2C0;

// ---- System / window / loop addresses ------------------------------------

inline constexpr std::uintptr_t CC_WINDOW_PROC_ADDR         = 0x40D4C0; // Location of WindowProc
inline constexpr std::uintptr_t CC_LOOP_START_ADDR          = 0x40D330; // Start of the main event loop
inline constexpr std::uintptr_t CC_SCREEN_WIDTH_ADDR        = 0x54D048; // The actual width of the main viewport

// ---- Match config (writable) ---------------------------------------------

inline constexpr std::uintptr_t CC_DAMAGE_LEVEL_ADDR       = 0x553FCC; // Damage level: default 2
inline constexpr std::uintptr_t CC_WIN_COUNT_VS_ADDR       = 0x553FDC; // Win count: default 2
inline constexpr std::uintptr_t CC_TIMER_SPEED_ADDR        = 0x553FD0; // Timer speed: default 2
inline constexpr std::uintptr_t CC_AUTO_REPLAY_SAVE_ADDR   = 0x553FE8; // Auto replay saving: 0 to disable, 1 to enable
inline constexpr std::uintptr_t CC_STAGE_ANIMATION_OFF_ADDR = 0x554124; // 1 if stage animations are off
inline constexpr std::uintptr_t CC_STAGE_SELECTOR_ADDR      = 0x74FD98; // Currently selected stage

// ---- Timers / pause / intro ----------------------------------------------

inline constexpr std::uintptr_t CC_WORLD_TIMER_ADDR       = 0x55D1D4; // Frame step timer, always counting up
inline constexpr std::uintptr_t CC_PAUSE_FLAG_ADDR        = 0x55D203; // 1 when paused
inline constexpr std::uintptr_t CC_SKIP_FRAMES_ADDR       = 0x55D25C; // Set to N to skip rendering for N frames
inline constexpr std::uintptr_t CC_ROUND_TIMER_ADDR       = 0x562A3C; // Counts down from 4752, may stop
inline constexpr std::uintptr_t CC_REAL_TIMER_ADDR        = 0x562A40; // Counts up from 0 after round start
inline constexpr std::uintptr_t CC_TRAINING_PAUSE_ADDR    = 0x562A64; // 1 when paused
inline constexpr std::uintptr_t CC_VERSUS_PAUSE_ADDR      = 0x564B30; // 0xFFFFFFFF when paused
inline constexpr std::uintptr_t CC_INTRO_STATE_ADDR       = 0x55D20B; // 2 (character intros), 1 (pre-game), 0 (in-game)

// ---- Round / wins / game point -------------------------------------------

inline constexpr std::uintptr_t CC_P1_GAME_POINT_FLAG_ADDR = 0x559548;
inline constexpr std::uintptr_t CC_P2_GAME_POINT_FLAG_ADDR = 0x55954C;
inline constexpr std::uintptr_t CC_P1_WINS_ADDR            = 0x559550;
inline constexpr std::uintptr_t CC_P2_WINS_ADDR            = 0x559580;
inline constexpr std::uintptr_t CC_ROUND_COUNT_ADDR        = 0x5550E0;

// ---- Status / FPS / perf -------------------------------------------------

inline constexpr std::uintptr_t CC_HIT_SPARKS_ADDR         = 0x67BD78;
inline constexpr std::uintptr_t CC_FPS_COUNTER_ADDR        = 0x774A70;
inline constexpr std::uintptr_t CC_PERF_FREQ_ADDR          = 0x774A80;
inline constexpr std::uintptr_t CC_SKIPPABLE_FLAG_ADDR     = 0x74D99C;
inline constexpr std::uintptr_t CC_ALIVE_FLAG_ADDR         = 0x76E650;
inline constexpr std::uintptr_t CC_REPLAY_CREATED_ADDR     = 0x774C30;
inline constexpr std::uintptr_t CC_D3DX9_OBJ_ADDR          = 0x76E7D4;  // IDirect3DDevice9 pointer
inline constexpr std::uintptr_t CC_MENU_STATE_COUNTER_ADDR = 0x767440;

// ---- Training mode -------------------------------------------------------

inline constexpr std::uintptr_t CC_DUMMY_STATUS_ADDR       = 0x74D7F8;

inline constexpr int32_t  CC_DUMMY_STATUS_STAND  = 0;
inline constexpr int32_t  CC_DUMMY_STATUS_JUMP   = 1;
inline constexpr int32_t  CC_DUMMY_STATUS_CROUCH = 2;
inline constexpr int32_t  CC_DUMMY_STATUS_CPU    = 3;
inline constexpr int32_t  CC_DUMMY_STATUS_MANUAL = 4;
inline constexpr int32_t  CC_DUMMY_STATUS_DUMMY  = 5;
inline constexpr int32_t  CC_DUMMY_STATUS_RECORD = -1;

inline constexpr std::uintptr_t CC_P1_COMBO_GUARD_ADDR    = 0x76E708;

// ---- Input write ---------------------------------------------------------

inline constexpr std::uintptr_t CC_PTR_TO_WRITE_INPUT_ADDR = 0x76E6AC; // Pointer to input struct
inline constexpr std::uintptr_t CC_P1_OFFSET_DIRECTION     = 0x18;
inline constexpr std::uintptr_t CC_P1_OFFSET_BUTTONS       = 0x24;
inline constexpr std::uintptr_t CC_P2_OFFSET_DIRECTION     = 0x2C;
inline constexpr std::uintptr_t CC_P2_OFFSET_BUTTONS       = 0x38;

// ---- Button masks (game input format) ------------------------------------

inline constexpr uint16_t CC_BUTTON_A      = 0x0010;
inline constexpr uint16_t CC_BUTTON_B      = 0x0020;
inline constexpr uint16_t CC_BUTTON_C      = 0x0008;
inline constexpr uint16_t CC_BUTTON_D      = 0x0004;
inline constexpr uint16_t CC_BUTTON_E      = 0x0080;
inline constexpr uint16_t CC_BUTTON_AB     = 0x0040;
inline constexpr uint16_t CC_BUTTON_START  = 0x0001;
inline constexpr uint16_t CC_BUTTON_FN1    = 0x0100; // Control dummy
inline constexpr uint16_t CC_BUTTON_FN2    = 0x0200; // Training reset
inline constexpr uint16_t CC_BUTTON_CONFIRM = 0x0400;
inline constexpr uint16_t CC_BUTTON_CANCEL  = 0x0800;
inline constexpr uint16_t CC_PLAYER_FACING  = 0x0002;

// ---- Game mode (CC_GAME_MODE_ADDR = 0x54EEE8) ----------------------------

inline constexpr std::uintptr_t CC_GAME_MODE_ADDR = 0x54EEE8;

inline constexpr uint32_t CC_GAME_MODE_STARTUP      = 65535;
inline constexpr uint32_t CC_GAME_MODE_OPENING      = 3;
inline constexpr uint32_t CC_GAME_MODE_TITLE        = 2;
inline constexpr uint32_t CC_GAME_MODE_LOADING_DEMO = 13;
inline constexpr uint32_t CC_GAME_MODE_HIGH_SCORES  = 11;
inline constexpr uint32_t CC_GAME_MODE_MAIN         = 25;
inline constexpr uint32_t CC_GAME_MODE_REPLAY       = 26;
inline constexpr uint32_t CC_GAME_MODE_CHARA_SELECT = 20;
inline constexpr uint32_t CC_GAME_MODE_LOADING      = 8;
inline constexpr uint32_t CC_GAME_MODE_IN_GAME      = 1;
inline constexpr uint32_t CC_GAME_MODE_RETRY        = 5;

// ---- Game state (CC_GAME_STATE_ADDR = 0x74d598) --------------------------

inline constexpr std::uintptr_t CC_GAME_STATE_ADDR = 0x74D598;

inline constexpr uint32_t CC_GAME_STATE_CHARA_INTRO  = 1;
inline constexpr uint32_t CC_GAME_STATE_INTRO_SKIP   = 101;
inline constexpr uint32_t CC_GAME_STATE_INTRO_MID    = 100;
inline constexpr uint32_t CC_GAME_STATE_INTRO_DONE   = 99;
inline constexpr uint32_t CC_GAME_STATE_CINTRO_END   = 12;
inline constexpr uint32_t CC_GAME_STATE_PREGAME_DONE = 2;

// ---- Character select addresses ------------------------------------------

inline constexpr std::uintptr_t CC_P1_SELECTOR_MODE_ADDR  = 0x74D8EC;
inline constexpr std::uintptr_t CC_P1_CHARA_SELECTOR_ADDR = 0x74D8F8;
inline constexpr std::uintptr_t CC_P1_CHARACTER_ADDR      = 0x74D8FC;
inline constexpr std::uintptr_t CC_P1_MOON_SELECTOR_ADDR  = 0x74D900;
inline constexpr std::uintptr_t CC_P1_COLOR_SELECTOR_ADDR = 0x74D904;

inline constexpr std::uintptr_t CC_P2_SELECTOR_MODE_ADDR  = 0x74D910;
inline constexpr std::uintptr_t CC_P2_CHARA_SELECTOR_ADDR = 0x74D91C;
inline constexpr std::uintptr_t CC_P2_CHARACTER_ADDR      = 0x74D920;
inline constexpr std::uintptr_t CC_P2_MOON_SELECTOR_ADDR  = 0x74D924;
inline constexpr std::uintptr_t CC_P2_COLOR_SELECTOR_ADDR = 0x74D928;

inline constexpr uint32_t CC_SELECT_CHARA = 0;
inline constexpr uint32_t CC_SELECT_MOON  = 1;
inline constexpr uint32_t CC_SELECT_COLOR = 2;

// ---- RNG state (rollback sync) -------------------------------------------

inline constexpr std::uintptr_t CC_RNG_STATE0_ADDR = 0x563778;
inline constexpr std::uintptr_t CC_RNG_STATE1_ADDR = 0x56377C;
inline constexpr std::uintptr_t CC_RNG_STATE2_ADDR = 0x564068;
inline constexpr std::uintptr_t CC_RNG_STATE3_ADDR = 0x564070;
inline constexpr int CC_RNG_STATE3_SIZE = 220;

// ---- Player struct (base 0x555130, size 0xAFC per player) ----------------

inline constexpr std::uintptr_t CC_PLR_STRUCT_SIZE = 0xAFC;

// P1
inline constexpr std::uintptr_t CC_P1_ENABLED_FLAG_ADDR   = 0x555130;
inline constexpr std::uintptr_t CC_P1_SEQUENCE_ADDR       = 0x555140;
inline constexpr std::uintptr_t CC_P1_SEQ_STATE_ADDR      = 0x555144;
inline constexpr std::uintptr_t CC_P1_HEALTH_ADDR         = 0x5551EC;
inline constexpr std::uintptr_t CC_P1_RED_HEALTH_ADDR     = 0x5551F0;
inline constexpr std::uintptr_t CC_P1_GUARD_BAR_ADDR      = 0x5551F4;
inline constexpr std::uintptr_t CC_P1_GUARD_QUALITY_ADDR  = 0x555208;
inline constexpr std::uintptr_t CC_P1_METER_ADDR          = 0x555210;
inline constexpr std::uintptr_t CC_P1_HEAT_ADDR           = 0x555214;
inline constexpr std::uintptr_t CC_P1_NO_INPUT_FLAG_ADDR  = 0x5552A7;
inline constexpr std::uintptr_t CC_P1_PUPPET_STATE_ADDR   = 0x5552A8;
inline constexpr std::uintptr_t CC_P1_X_POSITION_ADDR     = 0x555238;
inline constexpr std::uintptr_t CC_P1_Y_POSITION_ADDR     = 0x55523C;
inline constexpr std::uintptr_t CC_P1_X_PREV_POS_ADDR     = 0x555244;
inline constexpr std::uintptr_t CC_P1_Y_PREV_POS_ADDR     = 0x555248;
inline constexpr std::uintptr_t CC_P1_X_VELOCITY_ADDR     = 0x55524C;
inline constexpr std::uintptr_t CC_P1_Y_VELOCITY_ADDR     = 0x555250;
inline constexpr std::uintptr_t CC_P1_X_ACCELERATION_ADDR = 0x555254;
inline constexpr std::uintptr_t CC_P1_Y_ACCELERATION_ADDR = 0x555256;
inline constexpr std::uintptr_t CC_P1_SPRITE_ANGLE_ADDR   = 0x555430;
inline constexpr std::uintptr_t CC_P1_FACING_FLAG_ADDR    = 0x555444;
inline constexpr std::uintptr_t CC_P1_COMBO_OFFSET_ADDR   = 0x557E59;
inline constexpr std::uintptr_t CC_P1_COMBO_HIT_BASE_ADDR = 0x557E5C;

// P2 (= P1 + CC_PLR_STRUCT_SIZE)
inline constexpr std::uintptr_t CC_P2_ENABLED_FLAG_ADDR   = CC_P1_ENABLED_FLAG_ADDR  + CC_PLR_STRUCT_SIZE;
inline constexpr std::uintptr_t CC_P2_SEQUENCE_ADDR       = CC_P1_SEQUENCE_ADDR      + CC_PLR_STRUCT_SIZE;
inline constexpr std::uintptr_t CC_P2_SEQ_STATE_ADDR      = CC_P1_SEQ_STATE_ADDR     + CC_PLR_STRUCT_SIZE;
inline constexpr std::uintptr_t CC_P2_HEALTH_ADDR         = CC_P1_HEALTH_ADDR        + CC_PLR_STRUCT_SIZE;
inline constexpr std::uintptr_t CC_P2_RED_HEALTH_ADDR     = CC_P1_RED_HEALTH_ADDR    + CC_PLR_STRUCT_SIZE;
inline constexpr std::uintptr_t CC_P2_GUARD_BAR_ADDR      = CC_P1_GUARD_BAR_ADDR     + CC_PLR_STRUCT_SIZE;
inline constexpr std::uintptr_t CC_P2_GUARD_QUALITY_ADDR  = CC_P1_GUARD_QUALITY_ADDR + CC_PLR_STRUCT_SIZE;
inline constexpr std::uintptr_t CC_P2_METER_ADDR          = CC_P1_METER_ADDR         + CC_PLR_STRUCT_SIZE;
inline constexpr std::uintptr_t CC_P2_HEAT_ADDR           = CC_P1_HEAT_ADDR          + CC_PLR_STRUCT_SIZE;
inline constexpr std::uintptr_t CC_P2_NO_INPUT_FLAG_ADDR  = CC_P1_NO_INPUT_FLAG_ADDR + CC_PLR_STRUCT_SIZE;
inline constexpr std::uintptr_t CC_P2_PUPPET_STATE_ADDR   = CC_P1_PUPPET_STATE_ADDR  + CC_PLR_STRUCT_SIZE;
inline constexpr std::uintptr_t CC_P2_X_POSITION_ADDR     = CC_P1_X_POSITION_ADDR    + CC_PLR_STRUCT_SIZE;
inline constexpr std::uintptr_t CC_P2_Y_POSITION_ADDR     = CC_P1_Y_POSITION_ADDR    + CC_PLR_STRUCT_SIZE;
inline constexpr std::uintptr_t CC_P2_FACING_FLAG_ADDR    = CC_P1_FACING_FLAG_ADDR   + CC_PLR_STRUCT_SIZE;

// P3/P4 (puppets, = P2 + N * CC_PLR_STRUCT_SIZE)
inline constexpr std::uintptr_t CC_P3_ENABLED_FLAG_ADDR   = CC_P2_ENABLED_FLAG_ADDR  + CC_PLR_STRUCT_SIZE;
inline constexpr std::uintptr_t CC_P3_SEQUENCE_ADDR       = CC_P2_SEQUENCE_ADDR      + CC_PLR_STRUCT_SIZE;
inline constexpr std::uintptr_t CC_P3_NO_INPUT_FLAG_ADDR  = CC_P2_NO_INPUT_FLAG_ADDR + CC_PLR_STRUCT_SIZE;
inline constexpr std::uintptr_t CC_P3_PUPPET_STATE_ADDR   = CC_P2_PUPPET_STATE_ADDR  + CC_PLR_STRUCT_SIZE;

inline constexpr std::uintptr_t CC_P4_ENABLED_FLAG_ADDR   = CC_P3_ENABLED_FLAG_ADDR  + CC_PLR_STRUCT_SIZE;
inline constexpr std::uintptr_t CC_P4_NO_INPUT_FLAG_ADDR  = CC_P3_NO_INPUT_FLAG_ADDR + CC_PLR_STRUCT_SIZE;
inline constexpr std::uintptr_t CC_P4_PUPPET_STATE_ADDR   = CC_P3_PUPPET_STATE_ADDR  + CC_PLR_STRUCT_SIZE;

// ---- Camera / SFX / replay / hooks / fonts -------------------------------

inline constexpr std::uintptr_t CC_CAMERA_X_ADDR            = 0x564B14;
inline constexpr std::uintptr_t CC_CAMERA_Y_ADDR            = 0x564B18;

inline constexpr std::uintptr_t CC_SFX_ARRAY_ADDR           = 0x76E008;
inline constexpr int            CC_SFX_ARRAY_LEN            = 1500;
inline constexpr uint32_t       DX_MUTED_VOLUME             = 0xFFFFD8F0u;

inline constexpr std::uintptr_t CC_REPROUND_TBL_ENDPTR_ADDR = 0x77BF9C;

inline constexpr int CC_PRE_GAME_INTRO_FRAMES = 224;

// ASM hack addresses (prefixed MM for "modified memory")
inline constexpr std::uintptr_t MM_HOOK_CALL1_ADDR = 0x40D032;
inline constexpr std::uintptr_t MM_HOOK_CALL2_ADDR = 0x40D411;

// Allows for multiple instances of melty
inline constexpr std::uintptr_t MULTIPLE_MELTY = 0x40D25A;

// Sprite / font addresses (used by overlay — kept for future use)
inline constexpr std::uintptr_t BUTTON_SPRITE_TEX = 0x74D5E8;
inline constexpr std::uintptr_t FONT0             = 0x55D680;
inline constexpr std::uintptr_t FONT1             = 0x55D260;
inline constexpr std::uintptr_t FONT2             = 0x55DAA0;

inline constexpr int CC_SEQ_CROUCH_TRANSITION = 12;

// Display flags (used by trial — kept for reference)
inline constexpr std::uintptr_t CC_SHOW_ATTACK_DISPLAY = 0x5595B8;
inline constexpr std::uintptr_t CC_SHOW_INPUT_DISPLAY  = 0x5585F8;

// ---- IndexedFrame (frame + index packed in u64) --------------------------

union IndexedFrame {
    struct { uint32_t frame, index; } parts;
    uint64_t value;

    bool operator==(const IndexedFrame& o) const { return value == o.value; }
    bool operator!=(const IndexedFrame& o) const { return value != o.value; }
    bool operator<(const IndexedFrame& o) const { return value < o.value; }
    bool operator<=(const IndexedFrame& o) const { return value <= o.value; }
    bool operator>(const IndexedFrame& o) const { return value > o.value; }
    bool operator>=(const IndexedFrame& o) const { return value >= o.value; }
};

inline constexpr IndexedFrame MaxIndexedFrame = {{ UINT32_MAX, UINT32_MAX }};

// ---- Helpers --------------------------------------------------------------

// The CC_*_ADDR constants above are std::uintptr_t (integers), not
// pointers — this keeps them as plain compile-time constants without
// pulling in volatile semantics or weird type-punning. To dereference
// them, use these helpers:
//
//   *asU32(CC_P1_HEALTH_ADDR)            // read uint32_t
//   *asU32(CC_WORLD_TIMER_ADDR) = 0      // write uint32_t
//   *asU8(CC_INTRO_STATE_ADDR)           // read uint8_t
//
// This matches the pattern used in dll_main.cpp and dll_process_manager.cpp
// but centralizes the cast so callers don't have to repeat
// `*(uint32_t*)CC_..._ADDR` everywhere.

inline uint8_t*  asU8 (std::uintptr_t addr) { return reinterpret_cast<uint8_t*> (addr); }
inline uint16_t* asU16(std::uintptr_t addr) { return reinterpret_cast<uint16_t*>(addr); }
inline uint32_t* asU32(std::uintptr_t addr) { return reinterpret_cast<uint32_t*>(addr); }
inline int32_t*  asI32(std::uintptr_t addr) { return reinterpret_cast<int32_t*> (addr); }
inline float*    asF32(std::uintptr_t addr) { return reinterpret_cast<float*>   (addr); }
inline char*     asChr(std::uintptr_t addr) { return reinterpret_cast<char*>    (addr); }

inline const char* gameModeStr(uint32_t gameMode) {
    switch (gameMode) {
        case CC_GAME_MODE_STARTUP:      return "Startup";
        case CC_GAME_MODE_OPENING:      return "Opening";
        case CC_GAME_MODE_TITLE:        return "Title";
        case CC_GAME_MODE_LOADING_DEMO: return "Loading-demo";
        case CC_GAME_MODE_HIGH_SCORES:  return "High-scores";
        case CC_GAME_MODE_MAIN:         return "Main";
        case CC_GAME_MODE_CHARA_SELECT: return "Character-select";
        case CC_GAME_MODE_LOADING:      return "Loading";
        case CC_GAME_MODE_IN_GAME:      return "In-game";
        case CC_GAME_MODE_RETRY:        return "Retry";
        case CC_GAME_MODE_REPLAY:       return "Replay";
        default:                        return "Unknown game mode!";
    }
}

} // namespace caster::dll
