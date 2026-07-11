// src/common/ui_theme.hpp
//
// Dark UI theme for the caster launcher.
//
// What this module owns:
//   - Color palette (dark theme with red accents)
//   - Style push/pop helpers (frame/child rounding, padding, spacing)
//   - Gradient background drawn via ImDrawList
//   - Card container helper (child window with consistent bg/border/rounding)
//   - Compact sidebar nav button + bottom-anchored quit button
//   - Header logo (RE and CASTER)
//   - Primary CTA button (red) + secondary flat button
//
// What this module does NOT do: anything game-, netplay-, or session-related.
// Logic lives in src/exe/pages/.

#pragma once

#include <imgui.h>

#include <cstdint>
#include <string_view>

namespace caster::common::ui_theme {

// ---------------------------------------------------------------------------
// Plain color/vec types — same layout as ImVec4 / ImVec2 but explicit so
// the header stays decoupled from ImGui internals.
// ---------------------------------------------------------------------------
struct Color {
    float x, y, z, w;
    // Implicit conversion to ImVec4 — eliminates all manual
    // ImVec4(c.x, c.y, c.z, c.w) boilerplate at call sites.
    operator ImVec4() const noexcept { return ImVec4(x, y, z, w); }
};
struct Vec2 { float x, y; };

// ---------------------------------------------------------------------------
// Color palette
// ---------------------------------------------------------------------------

// Red brand accents
inline constexpr Color COL_RED     { 0.75f, 0.22f, 0.17f, 1.00f };
inline constexpr Color COL_RED_HOV { 0.66f, 0.18f, 0.15f, 1.00f };
inline constexpr Color COL_RED_ACT { 0.55f, 0.14f, 0.12f, 1.00f };
inline constexpr Color COL_RED_DIM { 0.45f, 0.13f, 0.10f, 1.00f };

// Backgrounds (vertical gradient: top=dark, bottom=mid)
inline constexpr Color COL_BG_DARK { 0.10f, 0.10f, 0.11f, 1.00f };
inline constexpr Color COL_BG_MID  { 0.16f, 0.16f, 0.18f, 1.00f };

// Card surface
inline constexpr Color COL_CARD     { 0.10f, 0.10f, 0.11f, 0.92f };
inline constexpr Color COL_CARD_BRD { 0.27f, 0.27f, 0.30f, 1.00f };

// Sidebar surface (slightly darker than card)
inline constexpr Color COL_SIDEBAR { 0.07f, 0.07f, 0.08f, 0.95f };

// Frame (input fields, combos)
inline constexpr Color COL_FRAME     { 0.18f, 0.18f, 0.20f, 1.00f };
inline constexpr Color COL_FRAME_HOV { 0.22f, 0.22f, 0.25f, 1.00f };
inline constexpr Color COL_FRAME_ACT { 0.26f, 0.26f, 0.30f, 1.00f };

// Header surface (very dark)
inline constexpr Color COL_HEADER_BAR { 0.05f, 0.05f, 0.06f, 0.95f };

// Text
inline constexpr Color COL_TEXT     { 0.92f, 0.92f, 0.94f, 1.00f };
inline constexpr Color COL_MUTED    { 0.50f, 0.50f, 0.55f, 1.00f };

// Navigation button (sidebar)
inline constexpr Color COL_NAV        { 0.00f, 0.00f, 0.00f, 0.00f };
inline constexpr Color COL_NAV_HOV    { 0.20f, 0.20f, 0.22f, 0.60f };
inline constexpr Color COL_NAV_ACTIVE { 0.30f, 0.10f, 0.08f, 0.50f };

// Transparent color (used to clear window bg so gradient shows through)
inline constexpr Color COL_TRANSPARENT { 0.0f, 0.0f, 0.0f, 0.0f };

// Status colors (used by pages for error/success/warning text)
inline constexpr Color COL_ERROR   { 1.00f, 0.40f, 0.40f, 1.00f };  // red text for errors
inline constexpr Color COL_SUCCESS { 0.40f, 0.90f, 0.40f, 1.00f };  // green text for success
inline constexpr Color COL_WARN    { 1.00f, 0.60f, 0.20f, 1.00f };  // orange text for warnings
inline constexpr Color COL_INFO    { 1.00f, 0.85f, 0.20f, 1.00f };  // yellow text for info/retry

// ---------------------------------------------------------------------------
// Layout constants (in pixels)
// ---------------------------------------------------------------------------
inline constexpr float SIDEBAR_W      = 56.0f;
inline constexpr float HEADER_H       = 64.0f;
inline constexpr float CONTENT_PAD    = 8.0f;
inline constexpr float CARD_ROUND     = 6.0f;
inline constexpr float CARD_PAD       = 8.0f;
inline constexpr float WINDOW_W       = 1024.0f;
inline constexpr float WINDOW_H       = 768.0f;
inline constexpr float NAV_BUTTON_PAD = 7.0f;  // horizontal padding for sidebar nav buttons

// ---------------------------------------------------------------------------
// Style/color stack wrappers — thin shims over ImGui's Push/Pop APIs.
// Kept inline in the header so unused-function warnings don't fire when only
// a subset is used by a given translation unit.
// ---------------------------------------------------------------------------
inline void pushStyleColor(ImGuiCol idx, Color col) {
    ImGui::PushStyleColor(idx, ImVec4(col.x, col.y, col.z, col.w));
}
inline void popStyleColor(int count = 1) { ImGui::PopStyleColor(count); }

// ---------------------------------------------------------------------------
// drawGradientBackground — full-window vertical gradient via ImDrawList.
// Call this INSIDE a Begin/End block (uses the window's draw list).
// ---------------------------------------------------------------------------
void drawGradientBackground();

// ---------------------------------------------------------------------------
// Card container — child window with consistent bg/border/rounding.
// Use as:
//   if (ui_theme::beginCard("MyCard", 400, 300, /*auto_resize_y=*/false)) {
//       ui_theme::cardTitle("Section Title");
//       ImGui::Text("...");
//       ui_theme::endCard();
//   }
// ---------------------------------------------------------------------------
bool beginCard(const char* id, float w, float h, bool auto_resize_y,
               ImGuiWindowFlags flags = 0);
void endCard();

// Centered card — handles the SetCursorPos centering math automatically.
// Uses WINDOW_W/WINDOW_H for centering.
bool beginCenteredCard(const char* id, float w, float h,
                       bool auto_resize_y = false,
                       ImGuiWindowFlags flags = 0);

// Small uppercase muted label inside a card.
void cardTitle(const char* text);

// ---------------------------------------------------------------------------
// Sidebar nav button — single letter, square, transparent until hover/active.
// Returns true if clicked this frame.
// ---------------------------------------------------------------------------
bool navButton(const char* letter, bool active);

// Primary CTA button — red, wider, framed. Use for the main action.
bool primaryButton(const char* label, float w, float h);

// Secondary button — flat dark, neutral. Use for cancel / secondary actions.
bool secondaryButton(const char* label, float w, float h);

// ---------------------------------------------------------------------------
// Header logo — "RE" white + "CASTER" red, drawn on the header bar via
// ImDrawList. Call inside a Begin/End block.
// ---------------------------------------------------------------------------
void drawLogo(float at_x, float at_y);

// ---------------------------------------------------------------------------
// Colored text helpers — push color, draw text, pop color. Eliminates
// the repeated PushStyleColor/Text/PopStyleColor pattern in pages.
// ---------------------------------------------------------------------------
void drawErrorText(const char* fmt, ...);
void drawSuccessText(const char* fmt, ...);
void drawWarnText(const char* fmt, ...);
void drawInfoText(const char* fmt, ...);

} // namespace caster::common::ui_theme
