// src/dll/overlay/playername_overlay.cpp
//
// Minimalist player-name overlay. Renders two small semi-transparent
// rectangles with player names in the top (or bottom) corners of the
// screen during netplay.
//
// Rendering uses the same D3D9 primitives as the info overlay:
//   - ID3DXFont for text (Tahoma 14, same font)
//   - device->Clear() for the background rectangle (fast, no VB needed)
//
// The font is shared with the info overlay (overlay_ui.cpp). We call
// overlay::presentFrameBegin() BEFORE playername::render() in the
// D3DHook callback, so the font is already initialized by the time we
// get here. If the font isn't ready (overlay never initialized), we
// skip rendering.

#include "playername_overlay.hpp"
#include "overlay_ui.hpp"
#include "primitives.hpp"
#include "game/addresses.hpp"
#include "../common/logger.hpp"

#ifndef NOMINMAX
#  define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d9.h>
#include <d3dx9.h>

#include <algorithm>
#include <cstring>

namespace caster::dll::overlay::playername {

// ----------------------------------------------------------------------------
// Constants
// ----------------------------------------------------------------------------

inline constexpr int kMaxNameChars = 16;
inline constexpr int kFontHeight   = 14;
inline constexpr int kPaddingX     = 6;
inline constexpr int kPaddingY     = 3;
inline constexpr int kMargin       = 8;  // distance from screen edge

inline constexpr D3DCOLOR kTextColor   = D3DCOLOR_XRGB(255, 255, 255);
inline constexpr D3DCOLOR kBgColor     = D3DCOLOR_ARGB(160, 0, 0, 0);

// ----------------------------------------------------------------------------
// State
// ----------------------------------------------------------------------------

static bool g_enabled       = true;   // from config
static bool g_positionTop   = true;   // from config (false = bottom)
static bool g_netplayActive = false;  // set every frame by setNetplayActive()
static bool g_userToggle    = true;   // toggled by hotkey '5'

static std::string g_p1Name;
static std::string g_p2Name;

// We need access to the ID3DXFont created by overlay_ui. Since overlay_ui
// doesn't expose its font, we create our own. This is a small overhead
// (one extra font object) but keeps the modules decoupled.
static ID3DXFont* g_font = nullptr;
static bool g_fontInitAttempted = false;

// ----------------------------------------------------------------------------
// Font management
// ----------------------------------------------------------------------------

static void ensureFont(IDirect3DDevice9* device) {
    if (g_font || g_fontInitAttempted) return;
    g_fontInitAttempted = true;

    if (!device) return;

    // Use the same font spec as the info overlay (Tahoma 14).
    HRESULT hr = D3DXCreateFontA(
        device,
        kFontHeight,
        5,                      // width
        600,                    // weight
        1,                      // mipmap levels
        FALSE,                  // italic
        ANSI_CHARSET,
        OUT_DEFAULT_PRECIS,
        ANTIALIASED_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE,
        "Tahoma",
        &g_font);

    if (SUCCEEDED(hr) && g_font) {
        caster::common::logger::info("playername: font initialized");
    } else {
        caster::common::logger::warn("playername: font init failed");
        g_font = nullptr;
    }
}

static void releaseFont() {
    if (g_font) {
        g_font->OnLostDevice();
        g_font->Release();
        g_font = nullptr;
    }
    g_fontInitAttempted = false;
}

// ----------------------------------------------------------------------------
// API
// ----------------------------------------------------------------------------

void init(bool enabled, bool positionTop) {
    g_enabled = enabled;
    g_positionTop = positionTop;
    caster::common::logger::info("playername: init (enabled={}, position={})",
        enabled, positionTop ? "top" : "bottom");
}

void setNetplayActive(bool active) {
    g_netplayActive = active;
}

void toggle() {
    g_userToggle = !g_userToggle;
    caster::common::logger::info("playername: toggled (now {})",
        g_userToggle ? "visible" : "hidden");
}

void setNames(const std::string& p1, const std::string& p2) {
    g_p1Name = p1;
    g_p2Name = p2;
    // Truncate to kMaxNameChars + ellipsis if too long.
    if (g_p1Name.size() > kMaxNameChars) {
        g_p1Name = g_p1Name.substr(0, kMaxNameChars - 1) + "\xE2\x80\xA6";  // UTF-8 ellipsis …
    }
    if (g_p2Name.size() > kMaxNameChars) {
        g_p2Name = g_p2Name.substr(0, kMaxNameChars - 1) + "\xE2\x80\xA6";
    }
}

bool isVisible() {
    return g_netplayActive && g_enabled && g_userToggle;
}

// ----------------------------------------------------------------------------
// Rendering
// ----------------------------------------------------------------------------

void render(IDirect3DDevice9* device) {
    if (!isVisible()) return;
    if (!device) return;

    ensureFont(device);
    if (!g_font) return;

    D3DVIEWPORT9 vp;
    device->GetViewport(&vp);

    // Measure text and draw background + text for each player.
    auto drawPlayer = [&](const std::string& name, int corner) {
        // corner: 0 = left, 1 = right
        if (name.empty()) return;

        // Measure the text.
        RECT textRect{};
        textRect.top = 0;
        textRect.left = 0;
        textRect.right = 1;
        textRect.bottom = kFontHeight;
        g_font->DrawTextA(nullptr, name.c_str(), -1, &textRect,
                          DT_CALCRECT | DT_LEFT, 0);

        const int textW = textRect.right - textRect.left;
        const int textH = textRect.bottom - textRect.top;

        // Background rectangle: text + padding.
        const int bgW = textW + 2 * kPaddingX;
        const int bgH = textH + 2 * kPaddingY;

        int bgX, bgY;
        if (corner == 0) {
            // Left corner.
            bgX = kMargin;
        } else {
            // Right corner.
            bgX = static_cast<int>(vp.Width) - bgW - kMargin;
        }

        if (g_positionTop) {
            bgY = kMargin;
        } else {
            bgY = static_cast<int>(vp.Height) - bgH - kMargin;
        }

        // Draw background (semi-transparent black).
        const D3DRECT bgRect = { bgX, bgY, bgX + bgW, bgY + bgH };
        device->Clear(1, &bgRect, D3DCLEAR_TARGET, kBgColor, 0, 0);

        // Draw text.
        RECT drawRect;
        drawRect.left = bgX + kPaddingX;
        drawRect.top = bgY + kPaddingY;
        drawRect.right = bgX + bgW - kPaddingX;
        drawRect.bottom = bgY + bgH - kPaddingY;
        g_font->DrawTextA(nullptr, name.c_str(), -1, &drawRect,
                          DT_LEFT | DT_VCENTER, kTextColor);
    };

    drawPlayer(g_p1Name, 0);  // left
    drawPlayer(g_p2Name, 1);  // right
}

// ----------------------------------------------------------------------------
// Device loss handling (called from overlay::invalidateDeviceObjects)
// ----------------------------------------------------------------------------

// This is called by overlay_ui when the D3D9 device is lost. We need to
// release our font too. We expose this via a friend declaration or just
// call it from overlay_ui. For now, we provide a public function.
void invalidateDeviceObjects() {
    releaseFont();
}

} // namespace caster::dll::overlay::playername
