// src/common/win32/window.cpp

#include "window.hpp"
#include "process.hpp"
#include "../logger.hpp"

#ifndef NOMINMAX
#  define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace caster::common::win32::window {

namespace {

// Context passed to EnumWindowsProc to find a window owned by our PID.
struct EnumContext {
    std::uint32_t target_pid;
    WindowHandle  found_hwnd;
};

BOOL CALLBACK enum_windows_proc(HWND hwnd, LPARAM lparam) {
    EnumContext* ctx = reinterpret_cast<EnumContext*>(lparam);
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != ctx->target_pid) return TRUE;  // keep enumerating

    // Skip invisible windows (e.g. message-only windows).
    if (!IsWindowVisible(hwnd)) return TRUE;

    // Skip windows that have a parent (we want top-level).
    if (GetParent(hwnd) != nullptr) return TRUE;

    // First visible top-level window of our PID wins.
    ctx->found_hwnd = reinterpret_cast<WindowHandle>(hwnd);
    return FALSE;  // stop enumerating
}

} // namespace

WindowHandle find_launcher_hwnd() {
    // First try GetActiveWindow — it's O(1) and works if our thread
    // currently has the active window.
    HWND active = GetActiveWindow();
    if (active) {
        return reinterpret_cast<WindowHandle>(active);
    }

    // Fallback: enumerate all top-level windows owned by our PID.
    EnumContext ctx{};
    ctx.target_pid = process::current_pid();
    EnumWindows(enum_windows_proc, reinterpret_cast<LPARAM>(&ctx));
    if (ctx.found_hwnd == kInvalidHandle) {
        logger::warn("find_launcher_hwnd: no top-level window found for PID {}",
                     ctx.target_pid);
    }
    return ctx.found_hwnd;
}

bool flash(WindowHandle hwnd) {
    if (hwnd == kInvalidHandle) return false;
    // FLASHW_ALL = flash both the window caption and the taskbar button.
    // FLASHW_TIMERNOFG = keep flashing until the window comes to the foreground.
    FLASHWINFO fi{};
    fi.cbSize    = sizeof(fi);
    fi.hwnd      = reinterpret_cast<HWND>(hwnd);
    fi.dwFlags   = FLASHW_ALL | FLASHW_TIMERNOFG;
    fi.uCount    = 5;
    fi.dwTimeout = 0;
    return FlashWindowEx(&fi) != 0;
}

bool beep() {
    // 0 = "OK" system sound (default notification).
    return MessageBeep(0) != 0;
}

} // namespace caster::common::win32::window
