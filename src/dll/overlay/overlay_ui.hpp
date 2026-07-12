// src/dll/overlay/overlay_ui.hpp
//
// DX9 in-game overlay API. Ported from CCCaster targets/DllOverlayUi.hpp.
// Trial-mode and mapping-mode APIs are omitted (categoria D — fora de escopo v1).
//
// The overlay renders a translucent black bar at the top of the game window
// with up to 3 columns of text (left / center / right) via D3D9 ID3DXFont.
// Rendering is driven by the D3DHook Present callback — see PresentFrameBegin()
// in overlay_ui.cpp. The overlay owns its own state machine with height
// animation (Disabled -> Enabling -> Enabled -> Disabling -> Disabled).
//
// Lifecycle:
//   1. lifecycle.cpp calls overlay::init() after HookDirectX() succeeds.
//      This only arms a "shouldInitDirectX" flag — the actual font/VB
//      creation happens lazily on the first Present call, when we have a
//      valid IDirect3DDevice9*.
//   2. lifecycle.cpp (or a future hotkey handler) calls enable()/disable()/
//      toggle() to control visibility.
//   3. dll_main.cpp's PresentFrameBegin() stub calls overlay::presentFrameBegin()
//      each frame; overlay::invalidateDeviceObjects() is called on D3D9 Reset.
//
// NOTE: On Wine, the D3DHook is never installed (see lifecycle.cpp), so none
// of these code paths execute. The overlay is a no-op there.

#pragma once

#include <array>
#include <cstdint>
#include <string>

// Forward declare the D3D9 type in the GLOBAL namespace (not inside
// caster::dll::overlay) so it refers to the real IDirect3DDevice9 from d3d9.h
// at link time. Putting the forward decl inside the namespace would create a
// *different* type `caster::dll::overlay::IDirect3DDevice9` that doesn't match
// the one d3d9.h defines.
struct IDirect3DDevice9;

namespace caster::dll::overlay {

// Default timeout (ms) for showMessage() — matches CCCaster's DEFAULT_MESSAGE_TIMEOUT.
inline constexpr int kDefaultMessageTimeout = 3000;

// Arm lazy DirectX init. Must be called after HookDirectX() succeeds.
// The actual font/vertex-buffer creation happens on the first Present call.
void init();

// Show / hide the overlay. These drive the state machine: Enabling/Disabling
// animate the bar height; Enabled/Disabled are the steady states.
void enable();
void disable();
void toggle();

bool isEnabled();
bool isDisabled();
bool isToggling();

// 3-column text accessors. Column 0 = left, 1 = center, 2 = right.
std::array<std::string, 3> getText();
void updateText();
void updateText(const std::array<std::string, 3>& text);

// Selector rectangles (used by the future controller-mapping overlay).
// For now they're kept in the API for parity but unused.
std::array<bool, 2> getShouldDrawSelector();
void updateSelector(uint8_t index, int position = 0, const std::string& line = "");

// Temporary centered message (overrides the 3-column text). Auto-dismisses
// after `timeout` ms. Resets its countdown while the game window is in the
// background (mirrors CCCaster behaviour).
void showMessage(const std::string& text, int timeout = kDefaultMessageTimeout);
void updateMessage();
bool isShowingMessage();

// Animated bar height (px). Drives the scaling transform on the background VB.
int getHeight();
int getNewHeight();

// D3DHook entry points — called from dll_main.cpp's global stubs.
// presentFrameBegin() does lazy init + renders the overlay text.
// invalidateDeviceObjects() releases the font + VB on device loss / Reset.
void presentFrameBegin(IDirect3DDevice9* device);
void invalidateDeviceObjects();

} // namespace caster::dll::overlay
