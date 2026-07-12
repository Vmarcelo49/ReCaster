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
#include <map>
#include <vector>

namespace caster::dll::overlay::keymapper {

namespace cm = caster::common::controller;

// ----------------------------------------------------------------------------
// Constants
// ----------------------------------------------------------------------------

// Action list — buttons only. Directions (Up/Down/Left/Right) are NOT
// bindable in the in-game keymapper; they must be configured via the
// launcher GUI. This avoids the problematic direction-capture flow that
// conflicted with D-pad navigation.
struct ActionEntry {
    const char* label;
    cm::BindingTarget target;
};

static constexpr std::array<ActionEntry, 9> kActions = {{
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

// VK codes that must NOT be captured as a keyboard binding (they're
// reserved for overlay hotkeys/navigation).
static bool isReservedKey(uint32_t vk) {
    return vk == kVK_Up || vk == kVK_Down || vk == kVK_Left ||
           vk == kVK_Right || vk == kVK_Return ||
           vk == '1' || vk == '2' || vk == '3' || vk == '4';  // overlay hotkeys
}

// ----------------------------------------------------------------------------
// State
// ----------------------------------------------------------------------------

enum class Mode { Inactive, Active };

struct PlayerState {
    bool assigned = false;            // has a device been assigned to this player?
    int  device_index = -1;           // -1 = keyboard, >=0 = SDL joystick index
    size_t overlayPos = 0;            // 0 = controller name (or "press..."), 1..N = action list, N+1 = Done
    bool done = false;                // player pressed Done — waiting for the other player
    bool modified = false;            // player changed at least one binding since keymapper opened
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

// Joystick edge-detection state. For each joystick index, we track the
// previous "direction mask" (bit 0=Left, 1=Right, 2=Down, 3=Up) so we can
// detect release-edges (direction was pressed last frame, released now).
// This is essential — without edge detection, navigation re-triggers every
// frame while a direction is held, making the selector fly to the bottom.
static std::map<int, uint8_t> g_joyPrevDirMask;  // joyIdx → prev mask

// Flag set by toggle() on activation, consumed by update() on the first
// frame. Used to pre-assign devices based on the current mapping.ini
// (so the overlay opens with devices already in their real positions).
static bool g_needsInit = false;

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

// Snapshot all 256 VK codes into g_kbPrevState. Called at the end of update()
// so the next frame's edge detection has a reference.
static void snapshotKeyboard() {
    for (int i = 0; i < 256; ++i) {
        g_kbPrevState[i] = (GetAsyncKeyState(i) & 0x8000) != 0;
    }
}

// Get the current direction mask for a joystick by index.
// Bit 0 = Left (4), bit 1 = Right (6), bit 2 = Down (2), bit 3 = Up (8).
// Opens a temporary SDL_Joystick handle (SDL allows multiple opens of the
// same index).
static uint8_t joyDirMask(int joyIndex, uint32_t deadzone) {
    SDL_Joystick* joy = SDL_JoystickOpen(joyIndex);
    if (!joy) return 0;

    uint8_t mask = 0;

    // Check hat first (most D-pads are hats).
    const int n_hats = SDL_JoystickNumHats(joy);
    for (int i = 0; i < n_hats; ++i) {
        const uint8_t state = SDL_JoystickGetHat(joy, i);
        if (state & SDL_HAT_LEFT)  mask |= 0x01;  // bit 0 = Left (4)
        if (state & SDL_HAT_RIGHT) mask |= 0x02;  // bit 1 = Right (6)
        if (state & SDL_HAT_DOWN)  mask |= 0x04;  // bit 2 = Down (2)
        if (state & SDL_HAT_UP)    mask |= 0x08;  // bit 3 = Up (8)
    }

    // If no hat direction, check analog stick.
    if (mask == 0) {
        const int x = SDL_JoystickGetAxis(joy, 0);
        const int y = SDL_JoystickGetAxis(joy, 1);
        const int dz = static_cast<int>(deadzone);
        if (x < -dz) mask |= 0x01;  // Left
        if (x >  dz) mask |= 0x02;  // Right
        if (y >  dz) mask |= 0x04;  // Down
        if (y < -dz) mask |= 0x08;  // Up
    }

    SDL_JoystickClose(joy);
    return mask;
}

// Check if a specific direction was RELEASED this frame on the given joystick
// (was pressed last frame, not pressed now). Uses g_joyPrevDirMask for state.
// bit: 0=Left, 1=Right, 2=Down, 3=Up.
static bool joyDirReleasedEdge(int joyIndex, uint8_t bit, uint32_t deadzone) {
    const uint8_t now  = joyDirMask(joyIndex, deadzone);
    const uint8_t prev = g_joyPrevDirMask[joyIndex];
    const uint8_t wasPressed = prev & (1 << bit);
    const uint8_t isPressed  = now  & (1 << bit);
    return wasPressed && !isPressed;
}

// Check if any non-direction button is currently pressed on a joystick.
static bool joyAnyButtonPressed(int joyIndex) {
    SDL_Joystick* joy = SDL_JoystickOpen(joyIndex);
    if (!joy) return false;
    const int n = SDL_JoystickNumButtons(joy);
    bool result = false;
    for (int j = 0; j < n; ++j) {
        if (SDL_JoystickGetButton(joy, j) != 0) { result = true; break; }
    }
    SDL_JoystickClose(joy);
    return result;
}

// ----------------------------------------------------------------------------
// API: toggle / isActive
// ----------------------------------------------------------------------------

void toggle() {
    if (g_mode == Mode::Inactive) {
        g_mode = Mode::Active;
        g_players = {};
        g_capturingKeyboard = {};
        g_pendingCaptureValid = {};
        g_joyPrevDirMask.clear();
        g_needsInit = true;  // pre-assign devices from mapping.ini on first update()
        // Enable the overlay so it's visible. The keymapper::update() will
        // call overlay::updateText() with the keymapper content.
        overlay::enable();
        caster::common::logger::info("keymapper: activated");
    } else {
        g_mode = Mode::Inactive;
        g_players = {};
        g_capturingKeyboard = {};
        g_pendingCaptureValid = {};
        g_joyPrevDirMask.clear();
        // Clear overlay selectors + reset text to default info-overlay content.
        overlay::updateSelector(0);
        overlay::updateSelector(1);
        overlay::updateText({ "ReCaster", "DX9 Overlay v0.1", "" });
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
    g_pendingCaptureValid = {};
    g_joyPrevDirMask.clear();
    // Clear overlay selectors + reset text to default info-overlay content.
    overlay::updateSelector(0);
    overlay::updateSelector(1);
    overlay::updateText({ "ReCaster", "DX9 Overlay v0.1", "" });
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

    // ---- Step 0: Pre-assign devices from mapping.ini (first frame only) ----
    // When the keymapper opens, read each player's device_index from their
    // ControllerMapping and pre-assign the device so the overlay starts in
    // the real state (not everything in the middle).
    if (g_needsInit) {
        g_needsInit = false;
        for (uint8_t i = 0; i < 2; ++i) {
            const int devIdx = mappings[i]->device_index;
            if (devIdx >= 0 && devIdx < n_joys) {
                // Joystick — exclusive assignment.
                g_players[i].assigned = true;
                g_players[i].device_index = devIdx;
                g_players[i].overlayPos = 0;
                caster::common::logger::info("keymapper: P{} pre-assigned device {} ({})",
                    i + 1, devIdx, g_joystickNames[devIdx]);
            } else if (devIdx < 0) {
                // Keyboard — shared, can be on both players.
                g_players[i].assigned = true;
                g_players[i].device_index = -1;
                g_players[i].overlayPos = 0;
                caster::common::logger::info("keymapper: P{} pre-assigned keyboard", i + 1);
            }
        }
    }

    // ---- Step 1: Device assignment (3-state: middle / P1 / P2) ----
    //
    // Mirrors CCCaster's handleMappingOverlay logic:
    //   - Press Right on a device:
    //       if it's P1's → unassign (back to middle)
    //       else if P2 slot empty → assign to P2
    //   - Press Left on a device:
    //       if it's P2's → unassign (back to middle)
    //       else if P1 slot empty → assign to P1
    //
    // Keyboard is shared — can be assigned to both P1 and P2 simultaneously.
    // Joysticks are exclusive — one joystick goes to only one player.
    //
    // All input uses RELEASE-EDGES so each physical press triggers once.

    constexpr uint32_t kScanDeadzone = 8000;

    auto findDevicePlayer = [&](int deviceIdx) -> int {
        // For joysticks (exclusive): return 0 or 1 or -1.
        // For keyboard (shared): return 0 if P1 has it, 1 if P2 has it,
        //   or -1 if neither. If both have it, returns 0 (first match).
        if (g_players[0].assigned && g_players[0].device_index == deviceIdx) return 0;
        if (g_players[1].assigned && g_players[1].device_index == deviceIdx) return 1;
        return -1;
    };

    auto unassignFromPlayer = [&](int player) {
        const int devIdx = g_players[player].device_index;
        g_players[player].assigned = false;
        g_players[player].device_index = -1;
        g_players[player].overlayPos = 0;
        caster::common::logger::info("keymapper: P{} unassigned device {} ({})",
            player + 1, devIdx,
            devIdx < 0 ? "keyboard" : g_joystickNames[devIdx].c_str());
    };

    auto assignDevice = [&](int deviceIdx, int player) {
        if (g_players[player].assigned) return;
        g_players[player].assigned = true;
        g_players[player].device_index = deviceIdx;
        g_players[player].overlayPos = 0;
        caster::common::logger::info("keymapper: P{} assigned device {} ({})",
            player + 1, deviceIdx,
            deviceIdx < 0 ? "keyboard" : g_joystickNames[deviceIdx].c_str());
    };

    // Keyboard (shared device — can be on both P1 and P2).
    {
        const bool p1HasKb = g_players[0].assigned && g_players[0].device_index == -1;
        const bool p2HasKb = g_players[1].assigned && g_players[1].device_index == -1;
        if (kbReleasedEdge(kVK_Left)) {
            if (p2HasKb) unassignFromPlayer(1);        // P2's kb + Left → middle
            else if (!p1HasKb) assignDevice(-1, 0);    // middle + Left → P1
        }
        if (kbReleasedEdge(kVK_Right)) {
            if (p1HasKb) unassignFromPlayer(0);        // P1's kb + Right → middle
            else if (!p2HasKb) assignDevice(-1, 1);    // middle + Right → P2
        }
    }

    // Joysticks (exclusive — one joystick per player max).
    for (int i = 0; i < n_joys; ++i) {
        const int owner = findDevicePlayer(i);
        if (joyDirReleasedEdge(i, 0, kScanDeadzone)) {  // Left released
            if (owner == 1) unassignFromPlayer(1);      // P2's joy + Left → middle
            else if (owner == -1 && !g_players[0].assigned) assignDevice(i, 0);  // → P1
        }
        if (joyDirReleasedEdge(i, 1, kScanDeadzone)) {  // Right released
            if (owner == 0) unassignFromPlayer(0);      // P1's joy + Right → middle
            else if (owner == -1 && !g_players[1].assigned) assignDevice(i, 1);  // → P2
        }
    }

    // ---- Step 2: Build overlay text (3 columns) ----
    // Center column lists only UNASSIGNED devices (assigned ones move to
    // the P1/P2 columns). Keyboard is listed if neither player has it;
    // joysticks are listed if not assigned to any player.
    std::array<std::string, 3> text;
    text[1] = "Controller Mapper";
    const bool kbAssigned = (g_players[0].assigned && g_players[0].device_index == -1)
                         || (g_players[1].assigned && g_players[1].device_index == -1);
    // Actually keyboard can be shared, so only hide it if BOTH have it.
    const bool p1HasKb = g_players[0].assigned && g_players[0].device_index == -1;
    const bool p2HasKb = g_players[1].assigned && g_players[1].device_index == -1;
    if (!p1HasKb && !p2HasKb)
        text[1] += "\n\nKeyboard";
    for (int j = 0; j < (int)g_joystickNames.size(); ++j) {
        // Skip joysticks assigned to either player — they show in the side columns.
        const bool joyAssigned = (g_players[0].assigned && g_players[0].device_index == j)
                              || (g_players[1].assigned && g_players[1].device_index == j);
        if (!joyAssigned)
            text[1] += "\n" + g_joystickNames[j];
    }

    const size_t controllersHeight = 3 + std::max((size_t)1, g_joystickNames.size());

    for (uint8_t i = 0; i < 2; ++i) {
        std::string& playerText = text[i ? 2 : 0];
        PlayerState& ps = g_players[i];

        if (!ps.assigned) {
            playerText = (i == 0)
                ? "Press Left to assign P1"
                : "Press Right to assign P2";
            overlay::updateSelector(i);
            continue;
        }

        // Build options list: [name, A, B, C, D, E, AB, Start, FN1, FN2, Done]
        std::vector<std::string> options;
        const bool isKeyboard = (ps.device_index < 0);
        const char* devName = isKeyboard
            ? "Keyboard"
            : (ps.device_index < (int)g_joystickNames.size()
                ? g_joystickNames[ps.device_index].c_str()
                : "(unknown)");
        options.push_back(devName);

        size_t headerHeight = std::max((size_t)3, controllersHeight);
        // Instructions: only button binding (no direction binding in-game).
        // Left/Right move the device between middle/P1/P2 (handled in Step 1).
        if (isKeyboard) {
            playerText = "Enter: bind  Up/Down: navigate\n";
            playerText += std::string(headerHeight - 2, '\n');
        } else {
            playerText = "Any button: bind  Up/Down: navigate\n";
            playerText += std::string(headerHeight - 2, '\n');
        }

        // Add button action rows.
        for (const auto& action : kActions) {
            const cm::InputBinding b = getBinding(*mappings[i], action.target);
            options.push_back(std::string(action.label) + " : " + b.label());
        }
        // Done option.
        options.push_back(isKeyboard ? "Done (press Enter)" : "Done (press any button)");

        // ---- Step 3: Handle input for this player ----
        // All input uses RELEASE-EDGES (was pressed last frame, released now).
        // This ensures each physical press only triggers one action — without
        // it, navigation re-triggers every frame while a key/direction is held,
        // making the selector fly to the bottom of the list instantly.
        const uint32_t deadzone = mappings[i]->deadzone;

        // Joystick direction release-edge helper for this player's device.
        // bit: 0=Left, 1=Right, 2=Down, 3=Up.
        auto playerJoyReleased = [&](uint8_t bit) -> bool {
            if (isKeyboard) return false;
            return joyDirReleasedEdge(ps.device_index, bit, deadzone);
        };
        // Joystick any-button pressed (continuous — used only for "Done" and
        // "capture button", both of which are one-shot actions that clear
        // state immediately so re-triggering is harmless).
        auto playerJoyAnyButton = [&]() -> bool {
            if (isKeyboard) return false;
            return joyAnyButtonPressed(ps.device_index);
        };

        // Apply pending keyboard capture (set by handleKeyEvent on keydown).
        if (g_pendingCaptureValid[i]) {
            if (ps.overlayPos >= 1 && ps.overlayPos < kActions.size() + 1) {
                const size_t actionIdx = ps.overlayPos - 1;
                setBinding(*mappings[i], kActions[actionIdx].target, g_pendingCapture[i]);
                ps.modified = true;
                caster::common::logger::info("keymapper: P{} applied captured key '{}' to {}",
                    i + 1, g_pendingCapture[i].label(), kActions[actionIdx].label);
            }
            g_pendingCaptureValid[i] = false;
        }

        // If capturing a keyboard key, skip navigation handling — the capture
        // happens in handleKeyEvent().
        if (g_capturingKeyboard[i]) {
            for (const auto& opt : options) playerText += "\n" + opt;
            overlay::updateSelector(i, headerHeight + ps.overlayPos, options[ps.overlayPos]);
            continue;
        }

        // Helper: check if a keyboard direction was released, considering
        // BOTH the arrow keys AND the player's mapped direction keys.
        // This way, if P2 has WASD mapped as directions, W/A/S/D also
        // navigate the overlay (not just arrow keys).
        // bit: 0=Left, 1=Right, 2=Down, 3=Up.
        auto playerKbDirReleased = [&](uint8_t bit) -> bool {
            if (!isKeyboard) return false;
            const auto& m = *mappings[i];
            uint32_t arrowKey = 0;
            switch (bit) {
                case 0: arrowKey = kVK_Left;  break;
                case 1: arrowKey = kVK_Right; break;
                case 2: arrowKey = kVK_Down;  break;
                case 3: arrowKey = kVK_Up;    break;
            }
            if (kbReleasedEdge(arrowKey)) return true;
            const cm::InputBinding* mapped = nullptr;
            switch (bit) {
                case 0: mapped = &m.left;  break;
                case 1: mapped = &m.right; break;
                case 2: mapped = &m.down;  break;
                case 3: mapped = &m.up;    break;
            }
            if (mapped && mapped->type == cm::InputType::KeyboardKey &&
                mapped->index != arrowKey &&
                mapped->index < 256) {
                if (kbReleasedEdge(mapped->index)) return true;
            }
            return false;
        };

        // Done detection: Enter (keyboard) or any button (joystick).
        // "Done" does NOT unassign the device — it just marks this player as
        // finished. When BOTH players are done, the keymapper saves the
        // mappings and returns to gameplay.
        if (ps.overlayPos + 1 == options.size()) {
            const bool donePressed = isKeyboard
                ? kbPressedEdge(kVK_Return)
                : playerJoyAnyButton();
            if (donePressed) {
                ps.done = true;
                ps.overlayPos = 0;
                caster::common::logger::info("keymapper: P{} done (waiting for other player)",
                    i + 1);
                // Auto-done: if the OTHER player hasn't modified anything
                // AND has no pending keyboard capture to apply, mark them as
                // done automatically. The pendingCapture check prevents
                // silently discarding an in-progress binding.
                const uint8_t other = 1 - i;
                if (!g_players[other].done && !g_players[other].modified &&
                    !g_pendingCaptureValid[other]) {
                    g_players[other].done = true;
                    caster::common::logger::info("keymapper: P{} auto-done (no modifications)",
                        other + 1);
                }
                // If both players are done, save + deactivate.
                if (g_players[0].done && g_players[1].done) {
                    cm::ControllerMapping* ms[2] = { mappings[0], mappings[1] };
                    deactivateAndSave(ms);
                    return;
                }
                // Set the "Done" text immediately so it shows this frame
                // (don't continue — fall through to the ps.done block below
                // which renders the waiting text).
            }
        }

        // If this player already pressed Done, skip further input handling
        // (they're waiting for the other player). They can cancel Done by
        // pressing Up or Down to go back to editing.
        if (ps.done) {
            // Up/Down cancels Done and resumes editing.
            // Respects player's mapped direction keys (e.g. W/S if WASD).
            if (playerKbDirReleased(3) || playerKbDirReleased(2) ||
                (!isKeyboard && (playerJoyReleased(3) || playerJoyReleased(2)))) {
                ps.done = false;
                caster::common::logger::info("keymapper: P{} cancelled Done, resuming edit",
                    i + 1);
            }
            // Render the "done" state text.
            playerText = "Done! Waiting for other player...\n";
            playerText += "Press Up/Down to resume editing";
            overlay::updateSelector(i);
            continue;
        }

        // Navigation + mapping actions. All use release-edges.
        // NOTE: Left/Right are NOT used here for unassign — that would
        // conflict with Step 1's device assignment (same release-edge would
        // fire both assign and unassign in the same frame, causing the
        // device to bounce back to middle immediately). Device unassign
        // is handled exclusively in Step 1 via the 3-state logic
        // (Left moves leftward: P2→middle→P1; Right moves rightward:
        // P1→middle→P2; same direction = stay).
        bool changedPosition = false;
        bool captureButton = false;

        // Keyboard: Enter on a button row → start capturing the next key.
        if (isKeyboard && ps.overlayPos >= 1 && ps.overlayPos <= kActions.size() &&
            kbReleasedEdge(kVK_Return)) {
            g_capturingKeyboard[i] = true;
            caster::common::logger::info("keymapper: P{} capturing key for {}",
                i + 1, kActions[ps.overlayPos - 1].label);
        }
        // Joystick: any button press while on a button row → capture that button.
        else if (!isKeyboard && ps.overlayPos >= 1 && ps.overlayPos <= kActions.size() &&
                 playerJoyAnyButton()) {
            captureButton = true;
        }
        // Move selector down: use release-edge (Down released).
        // Respects player's mapped Down key (e.g. 'S' if WASD).
        else if (playerKbDirReleased(2) || (!isKeyboard && playerJoyReleased(2))) {
            ps.overlayPos = (ps.overlayPos + 1) % options.size();
            changedPosition = true;
        }
        // Move selector up: use release-edge (Up released).
        // Respects player's mapped Up key (e.g. 'W' if WASD).
        else if (playerKbDirReleased(3) || (!isKeyboard && playerJoyReleased(3))) {
            ps.overlayPos = (ps.overlayPos + options.size() - 1) % options.size();
            changedPosition = true;
        }

        // Handle joystick button capture.
        if (captureButton && ps.overlayPos >= 1 && ps.overlayPos <= kActions.size()) {
            const size_t actionIdx = ps.overlayPos - 1;
            const auto& action = kActions[actionIdx];
            SDL_Joystick* tmp = SDL_JoystickOpen(ps.device_index);
            if (tmp) {
                cm::InputBinding captured = cm::poll_for_bind_input(tmp, ps.device_index);
                SDL_JoystickClose(tmp);
                if (captured.type != cm::InputType::None &&
                    captured.type != cm::InputType::SdlHat) {
                    setBinding(*mappings[i], action.target, captured);
                    ps.modified = true;
                    caster::common::logger::info("keymapper: P{} bound {} to {}",
                        i + 1, action.label, captured.label());
                }
            }
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
    // Snapshot joystick direction states for next frame's edge detection.
    // Use each player's own deadzone for their assigned device; for
    // unassigned joysticks, fall back to the default 8000.
    constexpr uint32_t kDefaultDeadzone = 8000;
    const int n_joys_snap = SDL_NumJoysticks();
    for (int j = 0; j < n_joys_snap; ++j) {
        uint32_t dz = kDefaultDeadzone;
        for (uint8_t p = 0; p < 2; ++p) {
            if (g_players[p].assigned && g_players[p].device_index == j) {
                dz = mappings[p]->deadzone;
                break;
            }
        }
        g_joyPrevDirMask[j] = joyDirMask(j, dz);
    }
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
