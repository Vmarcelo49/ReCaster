// src/common/win32/window.hpp
//
// Win32 window helpers: find the launcher's own HWND, flash it (to alert
// the user when a netplay handshake completes), and play a notification
// beep. Used by the netplay session.

#pragma once

#include <cstdint>

namespace caster::common::win32::window {

// Opaque HWND. Internally a HWND cast to uintptr_t.
using WindowHandle = std::uintptr_t;
inline constexpr WindowHandle kInvalidHandle = 0;

// Get the active window belonging to the current thread. Falls back to
// enumerating all top-level windows of the current process. Returns
// kInvalidHandle if nothing is found (e.g. running headless).
WindowHandle find_launcher_hwnd();

// Flash the window's taskbar button once (to alert the user). Used by
// the netplay session when a peer connects. Returns true on success.
bool flash(WindowHandle hwnd);

// Play the default Windows notification beep (MessageBeep). Used together
// with flash() for handshake-complete notifications.
bool beep();

} // namespace caster::common::win32::window
