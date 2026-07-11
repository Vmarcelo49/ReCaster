// src/dll/input/input_reader.cpp
//
// Reads SDL_Joystick + Win32 keyboard state, applies ControllerMapping,
// filters SOCD, converts to MBAA numpad+buttons format.
//
// Combines logic from:
// - our binder.cpp (poll_for_bind_input — same SDL/keyboard reading)
// - CCCaster's DllControllerUtils (SOCD filter + numpad conversion)
// - CCCaster's DllControllerManager::updateControls (input application)

#include "input_reader.hpp"
#include "game/addresses.hpp"
#include "../common/controller/mapping.hpp"

#include <SDL2/SDL_joystick.h>

#ifndef NOMINMAX
#  define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstring>
#include <fstream>

namespace caster::dll {

namespace cm = caster::common::controller;

// ---- Check if a single binding is active ---------------------------------

static bool is_binding_active(const cm::InputBinding& binding,
                               SDL_Joystick* joy,
                               uint32_t deadzone) {
    switch (binding.type) {
        case cm::InputType::None:
            return false;

        case cm::InputType::SdlButton:
            return joy && SDL_JoystickGetButton(joy, binding.index) != 0;

        case cm::InputType::SdlAxisPos:
            return joy && SDL_JoystickGetAxis(joy, binding.index) >
                          static_cast<int>(deadzone);

        case cm::InputType::SdlAxisNeg:
            return joy && SDL_JoystickGetAxis(joy, binding.index) <
                          -static_cast<int>(deadzone);

        case cm::InputType::SdlHat: {
            if (!joy) return false;
            const uint8_t hat_idx = cm::InputBinding::hat_index(binding.index);
            const uint8_t dir     = cm::InputBinding::hat_direction(binding.index);
            const uint8_t state   = SDL_JoystickGetHat(joy, hat_idx);
            switch (dir) {
                case 1: return (state & SDL_HAT_LEFT)  && (state & SDL_HAT_DOWN);  // DL
                case 2: return (state & SDL_HAT_DOWN);                            // D
                case 3: return (state & SDL_HAT_RIGHT) && (state & SDL_HAT_DOWN); // DR
                case 4: return (state & SDL_HAT_LEFT);                            // L
                case 6: return (state & SDL_HAT_RIGHT);                          // R
                case 7: return (state & SDL_HAT_LEFT)  && (state & SDL_HAT_UP);   // UL
                case 8: return (state & SDL_HAT_UP);                             // U
                case 9: return (state & SDL_HAT_RIGHT) && (state & SDL_HAT_UP);   // UR
                default: return false;
            }
        }

        case cm::InputType::KeyboardKey:
            return (GetAsyncKeyState(binding.index) & 0x8000) != 0;

        default:
            return false;
    }
}

// ---- SOCD filter ---------------------------------------------------------

DirState filter_socd(const DirState& raw, cm::SocdMode mode) {
    DirState out = raw;
    const uint8_t socd = static_cast<uint8_t>(mode);
    // bit 0 = L+R neutralize, bit 1 = U+D neutralize
    if ((socd & 0x01) && out.left && out.right) {
        out.left = out.right = false;
    }
    if ((socd & 0x02) && out.up && out.down) {
        out.up = out.down = false;
    }
    return out;
}

// ---- Read local input ----------------------------------------------------

GameInput read_local_input(SDL_Joystick* joy,
                           const cm::ControllerMapping& mapping) {
    GameInput result;

    // 1. Read raw direction bindings.
    DirState dir;
    dir.up    = is_binding_active(mapping.up,    joy, mapping.deadzone);
    dir.down  = is_binding_active(mapping.down,  joy, mapping.deadzone);
    dir.left  = is_binding_active(mapping.left,  joy, mapping.deadzone);
    dir.right = is_binding_active(mapping.right, joy, mapping.deadzone);

    // 2. Also read analog stick for direction (OR with D-pad/hat).
    if (joy) {
        const int x = SDL_JoystickGetAxis(joy, mapping.stick_x_axis);
        const int y = SDL_JoystickGetAxis(joy, mapping.stick_y_axis);
        const int dz = static_cast<int>(mapping.deadzone);
        if (x >  dz) dir.right = true;
        if (x < -dz) dir.left  = true;
        if (y >  dz) dir.down  = true;
        if (y < -dz) dir.up    = true;
    }

    // 3. Apply SOCD filter.
    dir = filter_socd(dir, mapping.socd_mode);

    // 4. Convert to numpad notation (5 = neutral, mapped to 0 for the game).
    //    Numpad: 7=UL 8=U 9=UR 4=L 5=N 6=R 1=DL 2=D 3=DR
    uint16_t numpad = 5;
    if (dir.up)    numpad = 8;
    if (dir.down)  numpad = 2;
    if (dir.left)  numpad = 4;
    if (dir.right) numpad = 6;
    if (dir.left && dir.up)    numpad = 7;
    if (dir.left && dir.down)  numpad = 1;
    if (dir.right && dir.up)   numpad = 9;
    if (dir.right && dir.down) numpad = 3;
    // Neutral (5) → 0 for the game.
    result.direction = (numpad == 5) ? 0 : numpad;

    // 5. Read button bindings.
    if (is_binding_active(mapping.a,     joy, mapping.deadzone)) result.buttons |= CC_BUTTON_A;
    if (is_binding_active(mapping.b,     joy, mapping.deadzone)) result.buttons |= CC_BUTTON_B;
    if (is_binding_active(mapping.c,     joy, mapping.deadzone)) result.buttons |= CC_BUTTON_C;
    if (is_binding_active(mapping.d,     joy, mapping.deadzone)) result.buttons |= CC_BUTTON_D;
    if (is_binding_active(mapping.e,     joy, mapping.deadzone)) result.buttons |= CC_BUTTON_E;
    if (is_binding_active(mapping.ab,    joy, mapping.deadzone)) result.buttons |= CC_BUTTON_AB;
    if (is_binding_active(mapping.start, joy, mapping.deadzone)) result.buttons |= CC_BUTTON_START;
    if (is_binding_active(mapping.fn1,   joy, mapping.deadzone)) result.buttons |= CC_BUTTON_FN1;
    if (is_binding_active(mapping.fn2,   joy, mapping.deadzone)) result.buttons |= CC_BUTTON_FN2;

    // Auto-set confirm/cancel based on A/B.
    if (result.buttons & CC_BUTTON_A) result.buttons |= CC_BUTTON_CONFIRM;
    if (result.buttons & CC_BUTTON_B) result.buttons |= CC_BUTTON_CANCEL;

    return result;
}

// ============================================================================
// Native MBAA keyboard reader (ported from zzcaster keyboard.zig)
// ============================================================================

namespace {

// VK codes loaded from MBAA.exe's keyboard config. Initialized once on first
// call. [0]=Down, [1]=Up, [2]=Left, [3]=Right, [4]=A/Confirm, [5]=B/Cancel,
// [6]=C, [7]=D, [8]=E, [9]=Start.
uint32_t g_vk_codes[10] = {};
bool g_kb_initialized = false;
bool g_kb_init_failed = false;

constexpr uint32_t kMapVkScToVkEx = 3;

bool init_native_keyboard() {
    if (g_kb_initialized || g_kb_init_failed) return g_kb_initialized;

    // Read the 10-byte keyboard config from MBAA.exe at the fixed offset.
    // The config is stored as scan codes; we convert to VK codes via
    // MapVirtualKey so GetAsyncKeyState can poll them.
    char exe_path[MAX_PATH] = {0};
    DWORD len = GetModuleFileNameA(nullptr, exe_path, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) {
        g_kb_init_failed = true;
        return false;
    }

    std::ifstream file(exe_path, std::ios::binary);
    if (!file.is_open()) {
        g_kb_init_failed = true;
        return false;
    }

    file.seekg(CC_KEYBOARD_CONFIG_OFFSET, std::ios::beg);
    uint8_t config[10] = {};
    file.read(reinterpret_cast<char*>(config), 10);
    if (file.gcount() < 10) {
        g_kb_init_failed = true;
        return false;
    }

    // Convert scan codes to VK codes.
    for (int i = 0; i < 10; ++i) {
        g_vk_codes[i] = MapVirtualKeyA(config[i], kMapVkScToVkEx);
    }

    g_kb_initialized = true;
    return true;
}

bool vk_down(uint32_t vk) {
    return (GetAsyncKeyState(static_cast<int>(vk)) & 0x8000) != 0;
}

} // namespace

GameInput read_native_keyboard() {
    GameInput result;

    if (!init_native_keyboard()) return result;

    // Read directions.
    bool up    = vk_down(g_vk_codes[1]);
    bool down  = vk_down(g_vk_codes[0]);
    bool left  = vk_down(g_vk_codes[2]);
    bool right = vk_down(g_vk_codes[3]);

    // SOCD: simultaneous opposing directions → neutral.
    if (up && down)   { up = false; down = false; }
    if (left && right) { left = false; right = false; }

    // Direction to numpad notation.
    uint16_t numpad = 5;
    if (up)         numpad = 8;
    else if (down)  numpad = 2;
    if (left)       numpad = (numpad > 1) ? numpad - 1 : 1;
    else if (right) numpad = (numpad < 9) ? numpad + 1 : 9;
    result.direction = (numpad == 5) ? 0 : numpad;

    // Read buttons.
    if (vk_down(g_vk_codes[4])) result.buttons |= CC_BUTTON_A;
    if (vk_down(g_vk_codes[5])) result.buttons |= CC_BUTTON_B;
    if (vk_down(g_vk_codes[6])) result.buttons |= CC_BUTTON_C;
    if (vk_down(g_vk_codes[7])) result.buttons |= CC_BUTTON_D;
    if (vk_down(g_vk_codes[8])) result.buttons |= CC_BUTTON_E;
    if (vk_down(g_vk_codes[9])) result.buttons |= CC_BUTTON_START;

    // A doubles as Confirm, B doubles as Cancel.
    if (result.buttons & CC_BUTTON_A) result.buttons |= CC_BUTTON_CONFIRM;
    if (result.buttons & CC_BUTTON_B) result.buttons |= CC_BUTTON_CANCEL;

    return result;
}

} // namespace caster::dll
