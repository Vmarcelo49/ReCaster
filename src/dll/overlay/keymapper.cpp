// src/dll/overlay/keymapper.cpp
//
// In-game controller mapping overlay. Ported from CCCaster's
// DllControllerManager::handleMappingOverlay(), adapted to ReCaster.
//
// Key differences from CCCaster:
//   - Uses caster::common::controller::ControllerMapping (our struct) instead
//     of CCCaster's Controller class. Bindings are accessed via mapping.a,
//     mapping.b, mapping.up, etc.
//   - Uses caster::common::controller::poll_for_bind_input() (already exists
//     for the launcher's click-to-bind flow) to capture new bindings.
//   - Uses our overlay::updateText()/updateSelector() API (3-column + 2
//     selectors) instead of CCCaster's DllOverlayUi.
//   - Saves directly to mapping.ini via caster::common::controller::save_mapping().
//
// State machine:
//   Inactive → SelectPlayer (waiting for device assignment) → Mapping
//   (navigating action list, capturing bindings) → Inactive (when both
//   players are done or unassigned).
//
// Per-player overlay layout (matches CCCaster):
//   P1 (left column, selector index 0):
//     "Press Left on P1 controller"     (no device assigned)
//     OR list of actions with current bindings (device assigned)
//   P2 (right column, selector index 1):
//     "Press Right on P2 controller"    (no device assigned)
//     OR list of actions with current bindings (device assigned)
//   Center column: list of all detected controllers (informational).

#include "keymapper.hpp"
#include "overlay_ui.hpp"
#include "../common/controller/mapping.hpp"
#include "../common/controller/binder.hpp"
#include "../common/logger.hpp"

#include <SDL2/SDL_joystick.h>

#ifndef NOMINMAX
#  define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <algorithm>
#include <vector>

namespace caster::dll::overlay::keymapper {

namespace cm = caster::common::controller;

// ----------------------------------------------------------------------------
// Constants
// ----------------------------------------------------------------------------

// Action list — order matters. First 4 are directions (keyboard-only mapping
// flow), rest are buttons (joystick + keyboard). Matches CCCaster's
// gameInputBits array.
struct ActionEntry {
    const char* label;
    cm::BindingTarget target;
};

static constexpr std::array<ActionEntry, 13> kActions = {{
    {"Up",    cm::BindingTarget::Up},
    {"Down",  cm::BindingTarget::Down},
    {"Left",  cm::BindingTarget::Left},
    {"Right", cm::BindingTarget::Right},
    {"A",     cm::BindingTarget::A},
    {"B",     cm::BindingTarget::B},
    {"C",     cm::BindingTarget::C},
    {"D",     cm::BindingTarget::D},
    {"E",     cm::BindingTarget::E},
    {"AB",    cm::BindingTarget::AB},
    {"Start", cm::BindingTarget::Start},
    {"FN1",   cm::BindingTarget::FN1},
    {"FN2",   cm::BindingTarget::FN2},
}};

// Win32 VK codes used for overlay navigation when the assigned device is
// a keyboard. We track prev-state ourselves to detect edges (GetAsyncKeyState
// alone doesn't give clean edge detection across our polling rate).
constexpr uint32_t kVK_Up    = VK_UP;
constexpr uint32_t kVK_Down  = VK_DOWN;
constexpr uint32_t kVK_Left  = VK_LEFT;
constexpr uint32_t kVK_Right = VK_RIGHT;
constexpr uint32_t kVK_Return = VK_RETURN;
constexpr uint32_t kVK_Delete = VK_DELETE;

// VK codes that must NOT be captured as a keyboard binding (they're reserved
// for overlay navigation). Matches CCCaster's filter in keyboardEvent().
static bool isReservedKey(uint32_t vk) {
    return vk == kVK_Up || vk == kVK_Down || vk == kVK_Left ||
           vk == kVK_Right || vk == kVK_Return || vk == kVK_Delete ||
           vk == '3' || vk == '4';  // overlay toggle hotkeys
}

// ----------------------------------------------------------------------------
// State
// ----------------------------------------------------------------------------

enum class Mode { Inactive, Active };

struct PlayerState {
    bool assigned = false;            // has a device been assigned to this player?
    int  device_index = -1;           // -1 = keyboard, >=0 = SDL joystick index
    size_t overlayPos = 0;            // 0 = controller name (or "press..."), 1..N = action list, N+1 = Done
    bool finishedMapping = false;     // set by controllerKeyMapped callback
    bool mappingDirection = false;    // true while waiting for a keyboard direction key
};

static Mode g_mode = Mode::Inactive;
static std::array<PlayerState, 2> g_players;
static std::string g_mappingPath;

// Cached snapshot of the connected SDL joysticks + their names. Refreshed
// every frame in update(). Index = SDL joystick index.
static std::vector<std::string> g_joystickNames;

// Keyboard edge-detection state. We poll GetAsyncKeyState each frame and
// compare to the previous snapshot to detect press-edges (for navigation)
// and release-edges (for "Enter released" semantics, matching CCCaster).
static std::array<bool, 256> g_kbPrevState{};

// While a keyboard player is in "capturing" state (overlayPos is on an
// action and the user pressed Enter), we capture the next non-reserved
// keydown as the binding. This flag tracks that.
static std::array<bool, 2> g_capturingKeyboard{};

// Pending keyboard captures (filled by handleKeyEvent, applied in update).
// The vkCode isn't available in update()'s scope, so handleKeyEvent() writes
// the complete InputBinding here and update() applies it to the mapping.
static std::array<cm::InputBinding, 2> g_pendingCapture{};
static std::array<bool, 2> g_pendingCaptureValid{};

// ----------------------------------------------------------------------------
// Helpers
// ----------------------------------------------------------------------------

// Get the current InputBinding for an action from a ControllerMapping.
static cm::InputBinding getBinding(const cm::ControllerMapping& m, cm::BindingTarget t) {
    switch (t) {
        case cm::BindingTarget::Up:    return m.up;
        case cm::BindingTarget::Down:  return m.down;
        case cm::BindingTarget::Left:  return m.left;
        case cm::BindingTarget::Right: return m.right;
        case cm::BindingTarget::A:     return m.a;
        case cm::BindingTarget::B:     return m.b;
        case cm::BindingTarget::C:     return m.c;
        case cm::BindingTarget::D:     return m.d;
        case cm::BindingTarget::E:     return m.e;
        case cm::BindingTarget::AB:    return m.ab;
        case cm::BindingTarget::Start: return m.start;
        case cm::BindingTarget::FN1:   return m.fn1;
        case cm::BindingTarget::FN2:   return m.fn2;
        default: return {};
    }
}

// Set a binding in a ControllerMapping.
static void setBinding(cm::ControllerMapping& m, cm::BindingTarget t, const cm::InputBinding& b) {
    switch (t) {
        case cm::BindingTarget::Up:    m.up = b; break;
        case cm::BindingTarget::Down:  m.down = b; break;
        case cm::BindingTarget::Left:  m.left = b; break;
        case cm::BindingTarget::Right: m.right = b; break;
        case cm::BindingTarget::A:     m.a = b; break;
        case cm::BindingTarget::B:     m.b = b; break;
        case cm::BindingTarget::C:     m.c = b; break;
        case cm::BindingTarget::D:     m.d = b; break;
        case cm::BindingTarget::E:     m.e = b; break;
        case cm::BindingTarget::AB:    m.ab = b; break;
        case cm::BindingTarget::Start: m.start = b; break;
        case cm::BindingTarget::FN1:   m.fn1 = b; break;
        case cm::BindingTarget::FN2:   m.fn2 = b; break;
        default: break;
    }
}

// Detect a press-edge for a VK code (was up last frame, down this frame).
static bool kbPressedEdge(uint32_t vk) {
    if (vk >= 256) return false;
    const bool now = (GetAsyncKeyState(vk) & 0x8000) != 0;
    const bool prev = g_kbPrevState[vk];
    return now && !prev;
}

// Detect a release-edge for a VK code (was down last frame, up this frame).
static bool kbReleasedEdge(uint32_t vk) {
    if (vk >= 256) return false;
    const bool now = (GetAsyncKeyState(vk) & 0x8000) != 0;
    const bool prev = g_kbPrevState[vk];
    return !now && prev;
}

// Detect a held state (down this frame; we don't care about prev — used for
// Up/Down navigation which allows held scrolling).
static bool kbHeld(uint32_t vk) {
    if (vk >= 256) return false;
    return (GetAsyncKeyState(vk) & 0x8000) != 0;
}

// Snapshot all 256 VK codes into g_kbPrevState. Called at the end of update()
// so the next frame's edge detection has a reference.
static void snapshotKeyboard() {
    for (int i = 0; i < 256; ++i) {
        g_kbPrevState[i] = (GetAsyncKeyState(i) & 0x8000) != 0;
    }
}

// Is a joystick direction pressed right now (numpad notation)?
// Used for navigation in the mapper overlay.
static bool joyDirPressed(SDL_Joystick* joy, uint8_t numpad_dir, uint32_t deadzone) {
    if (!joy) return false;

    // Check hat.
    const int n_hats = SDL_JoystickNumHats(joy);
    for (int i = 0; i < n_hats; ++i) {
        const uint8_t state = SDL_JoystickGetHat(joy, i);
        uint8_t derived = 5;
        if (state & SDL_HAT_UP)    derived = 8;
        if (state & SDL_HAT_DOWN)  derived = 2;
        if (state & SDL_HAT_LEFT)  derived = (derived == 8) ? 7 : (derived == 2) ? 1 : 4;
        if (state & SDL_HAT_RIGHT) derived = (derived == 8) ? 9 : (derived == 2) ? 3 : 6;
        if (derived == numpad_dir) return true;
    }

    // Check analog stick.
    const int x = SDL_JoystickGetAxis(joy, 0);  // stick_x_axis default = 0
    const int y = SDL_JoystickGetAxis(joy, 1);  // stick_y_axis default = 1
    const int dz = static_cast<int>(deadzone);
    const bool left  = x < -dz;
    const bool right = x >  dz;
    const bool up    = y < -dz;
    const bool down  = y >  dz;
    switch (numpad_dir) {
        case 8: return up && !left && !right;
        case 2: return down && !left && !right;
        case 4: return left && !up && !down;
        case 6: return right && !up && !down;
        default: return false;
    }
}

// Is any joystick button pressed right now? Used for "Done" selection and
// for triggering binding capture.
static bool joyAnyButtonPressed(SDL_Joystick* joy) {
    if (!joy) return false;
    const int n = SDL_JoystickNumButtons(joy);
    for (int i = 0; i < n; ++i) {
        if (SDL_JoystickGetButton(joy, i) != 0) return true;
    }
    return false;
}

// Count the number of options for a player's action list.
//   1 (controller name) + 13 (actions) + 1 (Done) = 15 for joystick
//   1 (controller name) + 13 (actions) + 1 (Done) = 15 for keyboard too
// (CCCaster's layout has 4 direction entries only for keyboard, but the
// total is the same since joystick shows buttons-only.)
static constexpr size_t kNumOptions = 1 + kActions.size() + 1;  // 15

// ----------------------------------------------------------------------------
// API: toggle / isActive
// ----------------------------------------------------------------------------

void toggle() {
    if (g_mode == Mode::Inactive) {
        g_mode = Mode::Active;
        g_players = {};  // reset both players
        // Disable the info overlay while the mapper is active — they share
        // the same screen space. The mapper will call overlay::updateText()
        // with its own content.
        overlay::disable();
        caster::common::logger::info("keymapper: activated");
    } else {
        g_mode = Mode::Inactive;
        // Re-enable the info overlay.
        overlay::enable();
        caster::common::logger::info("keymapper: deactivated");
    }
}

bool isActive() {
    return g_mode == Mode::Active;
}

// ----------------------------------------------------------------------------
// Per-frame update
// ----------------------------------------------------------------------------

static void deactivateAndSave(cm::ControllerMapping* mappings[2]) {
    // Save both players' mappings to disk.
    if (!g_mappingPath.empty() && mappings[0] && mappings[1]) {
        if (cm::save_mapping(g_mappingPath, *mappings[0], *mappings[1])) {
            caster::common::logger::info("keymapper: saved mapping.ini to {}", g_mappingPath);
        } else {
            caster::common::logger::warn("keymapper: failed to save mapping.ini to {}", g_mappingPath);
        }
    }
    g_mode = Mode::Inactive;
    g_players = {};
    g_capturingKeyboard = {};
    // Clear overlay selectors + re-enable info overlay.
    overlay::updateSelector(0);
    overlay::updateSelector(1);
    overlay::enable();
}

void update(std::array<SDL_Joystick*, 2> joys,
            std::array<cm::ControllerMapping*, 2> mappings,
            const std::string& mappingPath) {
    if (g_mode != Mode::Active) return;

    g_mappingPath = mappingPath;

    // Refresh joystick names.
    const int n_joys = SDL_NumJoysticks();
    g_joystickNames.clear();
    g_joystickNames.reserve(n_joys);
    for (int i = 0; i < n_joys; ++i) {
        const char* name = SDL_JoystickNameForIndex(i);
        g_joystickNames.emplace_back(name ? name : "(unknown)");
    }

    // ---- Step 1: Device assignment (Left/Right) ----
    // For each connected joystick + keyboard, check if the user pressed
    // Left (→ assign to P1) or Right (→ assign to P2).
    //
    // We use edge detection on joystick directions (prev state was centered,
    // now pressed) to avoid re-triggering every frame. For simplicity, we
    // poll all joysticks and all relevant keyboard arrow keys.

    auto assignDevice = [&](int deviceIdx, int player) {
        if (g_players[player].assigned) return;
        g_players[player].assigned = true;
        g_players[player].device_index = deviceIdx;
        g_players[player].overlayPos = 0;
        caster::common::logger::info("keymapper: P{} assigned device {} ({})",
            player + 1, deviceIdx,
            deviceIdx < 0 ? "keyboard" : g_joystickNames[deviceIdx].c_str());
    };

    // Check each joystick for Left (→ P1) / Right (→ P2).
    for (int i = 0; i < n_joys; ++i) {
        // We don't have an open handle for arbitrary joysticks — only for
        // the ones passed in joys[0]/joys[1]. For now, only those two can
        // be assigned. (Future: open a temporary handle to scan all devs.)
        // This is a known limitation vs CCCaster; acceptable for v1.
    }

    // Check keyboard arrows for device assignment.
    if (!g_players[0].assigned && kbPressedEdge(kVK_Left)) {
        assignDevice(-1, 0);
    }
    if (!g_players[1].assigned && kbPressedEdge(kVK_Right)) {
        assignDevice(-1, 1);
    }

    // Check the two open joysticks.
    for (int p = 0; p < 2; ++p) {
        if (g_players[p].assigned) continue;
        if (!joys[p]) continue;
        const int devIdx = (p == 0) ? 0 : 1;  // simplistic: joys[0]→P1, joys[1]→P2
        // Use a reasonable default deadzone for edge detection.
        if (mappings[p] && joyDirPressed(joys[p], 4, mappings[p]->deadzone)) {
            assignDevice(devIdx, 0);
        } else if (mappings[p] && joyDirPressed(joys[p], 6, mappings[p]->deadzone)) {
            assignDevice(devIdx, 1);
        }
    }

    // ---- Step 2: Build overlay text (3 columns) ----
    std::array<std::string, 3> text;
    text[1] = "Controller Mapper\n";
    for (const auto& name : g_joystickNames)
        text[1] += "\n" + name;
    if (g_joystickNames.empty())
        text[1] += "\n(no joysticks)";

    const size_t controllersHeight = 3 + std::max((size_t)1, g_joystickNames.size());

    for (uint8_t i = 0; i < 2; ++i) {
        std::string& playerText = text[i ? 2 : 0];
        PlayerState& ps = g_players[i];

        if (!ps.assigned) {
            playerText = (i == 0)
                ? "Press Left on P1 controller"
                : "Press Right on P2 controller";
            overlay::updateSelector(i);
            continue;
        }

        // Build options list: [name, Up, Down, ..., FN2, Done]
        std::vector<std::string> options;
        const bool isKeyboard = (ps.device_index < 0);
        const char* devName = isKeyboard
            ? "Keyboard"
            : (ps.device_index < (int)g_joystickNames.size()
                ? g_joystickNames[ps.device_index].c_str()
                : "(unknown)");
        options.push_back(devName);

        size_t headerHeight = std::max((size_t)3, controllersHeight);
        if (isKeyboard) {
            playerText = "Press Enter to set a direction key\n";
            playerText += "Press " + std::string(i == 0 ? "Left" : "Right") + " to delete a key\n";
            playerText += std::string(headerHeight - 3, '\n');
        } else {
            playerText = "Press " + std::string(i == 0 ? "Left" : "Right") + " to delete a key\n";
            playerText += std::string(headerHeight - 2, '\n');
        }

        // Add action rows.
        for (const auto& action : kActions) {
            const cm::InputBinding b = getBinding(*mappings[i], action.target);
            options.push_back(std::string(action.label) + " : " + b.label());
        }
        // Done option.
        options.push_back(isKeyboard ? "Done (press Enter)" : "Done (press any button)");

        // ---- Step 3: Handle input for this player ----
        SDL_Joystick* joy = isKeyboard ? nullptr : joys[ps.device_index];
        const uint32_t deadzone = mappings[i]->deadzone;

        // Apply pending keyboard capture (set by handleKeyEvent on keydown).
        if (g_pendingCaptureValid[i]) {
            if (ps.overlayPos >= 1 && ps.overlayPos < kActions.size() + 1) {
                const size_t actionIdx = ps.overlayPos - 1;
                setBinding(*mappings[i], kActions[actionIdx].target, g_pendingCapture[i]);
                caster::common::logger::info("keymapper: P{} applied captured key '{}' to {}",
                    i + 1, g_pendingCapture[i].label(), kActions[actionIdx].label);
            }
            g_pendingCaptureValid[i] = false;
        }

        // If capturing a keyboard key, skip navigation handling — the capture
        // happens in handleKeyEvent().
        if (g_capturingKeyboard[i]) {
            // Just update the selector + text; key capture is event-driven.
            for (const auto& opt : options) playerText += "\n" + opt;
            overlay::updateSelector(i, headerHeight + ps.overlayPos, options[ps.overlayPos]);
            continue;
        }

        // Done detection.
        if (ps.overlayPos + 1 == options.size()) {
            const bool donePressed = isKeyboard
                ? kbPressedEdge(kVK_Return)
                : joyAnyButtonPressed(joy);
            if (donePressed) {
                ps.overlayPos = 0;
                ps.assigned = false;
                // If both players are unassigned, save + deactivate.
                if (!g_players[0].assigned && !g_players[1].assigned) {
                    cm::ControllerMapping* ms[2] = { mappings[0], mappings[1] };
                    deactivateAndSave(ms);
                    return;
                }
                continue;
            }
        }

        // Navigation + mapping actions.
        bool deleteMapping = false, mapDirection = false, changedPosition = false;

        // Delete (P1: Left, P2: Right)
        const uint8_t deleteDir = (i == 0) ? 4 : 6;
        const uint32_t deleteKey = (i == 0) ? kVK_Left : kVK_Right;
        if ((joy && joyDirPressed(joy, deleteDir, deadzone)) ||
            (isKeyboard && kbPressedEdge(deleteKey))) {
            deleteMapping = true;
        }
        // Keyboard: Enter on a direction row → start capturing.
        else if (isKeyboard && ps.overlayPos >= 1 && ps.overlayPos <= 4 &&
                 (kbReleasedEdge(kVK_Return) || kbPressedEdge(kVK_Delete))) {
            if (kbReleasedEdge(kVK_Return)) mapDirection = true;
            else deleteMapping = true;
        }
        // Move selector down.
        else if ((joy && joyDirPressed(joy, 2, deadzone)) ||
                 (isKeyboard && kbHeld(kVK_Down))) {
            ps.overlayPos = (ps.overlayPos + 1) % options.size();
            changedPosition = true;
        }
        // Move selector up.
        else if ((joy && joyDirPressed(joy, 8, deadzone)) ||
                 (isKeyboard && kbHeld(kVK_Up))) {
            ps.overlayPos = (ps.overlayPos + options.size() - 1) % options.size();
            changedPosition = true;
        }

        if (deleteMapping || mapDirection || changedPosition || ps.finishedMapping) {
            if (ps.overlayPos >= 1 && ps.overlayPos < kActions.size() + 1) {
                const size_t actionIdx = ps.overlayPos - 1;
                const auto& action = kActions[actionIdx];

                if (deleteMapping) {
                    setBinding(*mappings[i], action.target, {});
                    caster::common::logger::info("keymapper: P{} deleted binding for {}",
                        i + 1, action.label);
                }
                else if (isKeyboard && (mapDirection || ps.finishedMapping) && actionIdx < 4) {
                    // Start capturing the next keyboard key for this direction.
                    g_capturingKeyboard[i] = true;
                    caster::common::logger::info("keymapper: P{} capturing key for {}",
                        i + 1, action.label);
                }
                else if (!isKeyboard && actionIdx >= 4) {
                    // Joystick button capture: poll until a non-direction input is detected.
                    // We do this synchronously: call poll_for_bind_input, and if it returns
                    // a button/axis binding, apply it immediately.
                    cm::InputBinding captured = cm::poll_for_bind_input(joy, ps.device_index);
                    if (captured.type != cm::InputType::None &&
                        captured.type != cm::InputType::SdlHat) {
                        // Only accept buttons + axes (not hats — hats are for directions).
                        setBinding(*mappings[i], action.target, captured);
                        caster::common::logger::info("keymapper: P{} bound {} to {}",
                            i + 1, action.label, captured.label());
                    }
                }
                else {
                    // Cancel any in-progress capture.
                    g_capturingKeyboard[i] = false;
                }
            }
            else {
                g_capturingKeyboard[i] = false;
            }
            ps.finishedMapping = false;
        }

        // Build display text.
        if (ps.overlayPos == 0) {
            playerText = "Press Up or Down to set keys";
            playerText += std::string(controllersHeight, '\n');
            playerText += devName;
            overlay::updateSelector(i, controllersHeight, devName);
        } else {
            for (const auto& opt : options) playerText += "\n" + opt;
            overlay::updateSelector(i, headerHeight + ps.overlayPos, options[ps.overlayPos]);
        }
    }

    overlay::updateText(text);
    snapshotKeyboard();
}

// ----------------------------------------------------------------------------
// Keyboard event hook
// ----------------------------------------------------------------------------

bool handleKeyEvent(uint32_t vkCode, bool isDown) {
    if (g_mode != Mode::Active) return false;

    // If any player is capturing a keyboard key, consume the event and
    // assign it to the action being mapped.
    for (uint8_t i = 0; i < 2; ++i) {
        if (!g_capturingKeyboard[i]) continue;
        if (!isDown) continue;  // only capture on keydown
        if (isReservedKey(vkCode)) {
            caster::common::logger::info("keymapper: ignoring reserved key 0x{:02X}", vkCode);
            return true;  // consume but don't assign
        }

        // Store the captured binding; update() will apply it to the mapping
        // (we don't have access to mappings[] here).
        cm::InputBinding b;
        b.type = cm::InputType::KeyboardKey;
        b.index = static_cast<std::uint16_t>(vkCode);
        g_pendingCapture[i] = b;
        g_pendingCaptureValid[i] = true;
        g_capturingKeyboard[i] = false;

        // Log which action it's for (read-only — we can derive from overlayPos).
        if (g_players[i].overlayPos >= 1 && g_players[i].overlayPos < kActions.size() + 1) {
            const size_t actionIdx = g_players[i].overlayPos - 1;
            caster::common::logger::info("keymapper: P{} captured key 0x{:02X} for {}",
                i + 1, vkCode, kActions[actionIdx].label);
        }
        return true;  // consume
    }

    return false;
}

} // namespace caster::dll::overlay::keymapper
