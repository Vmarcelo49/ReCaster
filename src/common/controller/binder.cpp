// src/common/controller/binder.cpp

#include "binder.hpp"
#include "../logger.hpp"

#include <SDL2/SDL_joystick.h>

#ifndef NOMINMAX
#  define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace caster::common::controller {

namespace {

// Axis thresholds (raw SDL units, range -32768..32767).
// Matches zzcaster: triggers report -32768 at rest, so we skip that value.
constexpr int kAxisPosThreshold    = 20000;
constexpr int kAxisNegThreshold    = -20000;
constexpr int kAxisNegFloor        = -32000;  // exclude near-full-negative
constexpr int kAxisRestValue       = -32768;  // skip triggers at rest

// Compute the numpad direction code from an SDL hat state.
//   SDL_HAT_UP    = 0x01  →  dir = 8
//   SDL_HAT_DOWN  = 0x04  →  dir = 2
//   SDL_HAT_LEFT  = 0x08  →  dir -= 1 (saturate at 1)
//   SDL_HAT_RIGHT = 0x02  →  dir += 1 (saturate at 9)
// Center = 5 (never bound).
std::uint8_t hat_dir_from_sdl(std::uint8_t hat_state) {
    if (hat_state == SDL_HAT_CENTERED) return 5;

    std::uint8_t dir = 5;
    if (hat_state & SDL_HAT_UP)    dir = 8;
    if (hat_state & SDL_HAT_DOWN)  dir = 2;
    // Now combine horizontally. Saturate so we don't go below 1 or above 9.
    if (hat_state & SDL_HAT_LEFT) {
        if (dir > 1) dir -= 1;   // 8→7, 2→1, 5→4
        else         dir = 1;
    }
    if (hat_state & SDL_HAT_RIGHT) {
        if (dir < 9) dir += 1;   // 8→9, 2→3, 5→6
        else         dir = 9;
    }
    return dir;
}

} // namespace

InputBinding poll_for_bind_input(SDL_Joystick* joy, int device_idx) {
    InputBinding result;  // None by default

    if (device_idx >= 0) {
        // ---- Joystick --------------------------------------------------
        if (!joy) return result;

        // 1. Buttons (highest priority).
        const int n_buttons = SDL_JoystickNumButtons(joy);
        for (int i = 0; i < n_buttons; ++i) {
            if (SDL_JoystickGetButton(joy, i) != 0) {
                result.type  = InputType::SdlButton;
                result.index = static_cast<std::uint16_t>(i);
                return result;
            }
        }

        // 2. Axes.
        const int n_axes = SDL_JoystickNumAxes(joy);
        for (int i = 0; i < n_axes; ++i) {
            const int val = SDL_JoystickGetAxis(joy, i);
            if (val == kAxisRestValue) continue;  // trigger at rest
            if (val > kAxisPosThreshold) {
                result.type  = InputType::SdlAxisPos;
                result.index = static_cast<std::uint16_t>(i);
                return result;
            }
            if (val < kAxisNegThreshold && val > kAxisNegFloor) {
                result.type  = InputType::SdlAxisNeg;
                result.index = static_cast<std::uint16_t>(i);
                return result;
            }
        }

        // 3. Hats.
        const int n_hats = SDL_JoystickNumHats(joy);
        for (int i = 0; i < n_hats; ++i) {
            const std::uint8_t hat_state = SDL_JoystickGetHat(joy, i);
            const std::uint8_t dir = hat_dir_from_sdl(hat_state);
            if (dir == 5) continue;  // centered
            result.type  = InputType::SdlHat;
            result.index = InputBinding::pack_hat(
                static_cast<std::uint8_t>(i), dir);
            return result;
        }
    } else {
        // ---- Keyboard --------------------------------------------------
        // Range 0x08..0xFE — skip mouse buttons (0x01..0x05), VK_CANCEL (0x03),
        // and other undefined codes 0x06..0x07.
        for (int vk = 0x08; vk <= 0xFE; ++vk) {
            const SHORT state = GetAsyncKeyState(vk);
            // High bit (0x8000) = "currently pressed".
            if (state & static_cast<SHORT>(0x8000)) {
                result.type  = InputType::KeyboardKey;
                result.index = static_cast<std::uint16_t>(vk);
                logger::info("binder: keyboard key detected vk=0x{:02x}", vk);
                return result;
            }
        }
    }

    return result;  // None — nothing pressed
}

} // namespace caster::common::controller
