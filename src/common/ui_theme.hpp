// src/common/ui_theme.hpp
//
// Modern dark UI theme for caster. Ported from zzcaster's
// `src/launcher/ui_theme.zig`. Visual identity is identical to the
// Zig version so the two launchers look the same side-by-side.
//
// What this module owns:
//   - Color palette (dark theme with red accents)
//   - Style push/pop helpers (frame/child rounding, padding, spacing)
//   - Gradient background drawn via ImDrawList
//   - Card container helper (child window with consistent bg/border/rounding)
//   - Compact sidebar nav button + bottom-anchored quit button
//   - Header logo (ZZ + CASTER)
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
struct Color { float x, y, z, w; };
struct Vec2  { float x, y; };

// ---------------------------------------------------------------------------
// Color palette — keep names and values identical to zzcaster
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
inline constexpr Color COL_TEXT_DIM { 0.35f, 0.35f, 0.38f, 1.00f };

// Navigation button (sidebar)
inline constexpr Color COL_NAV        { 0.00f, 0.00f, 0.00f, 0.00f };
inline constexpr Color COL_NAV_HOV    { 0.20f, 0.20f, 0.22f, 0.60f };
inline constexpr Color COL_NAV_ACTIVE { 0.30f, 0.10f, 0.08f, 0.50f };

// Transparent color (used to clear window bg so gradient shows through)
inline constexpr Color COL_TRANSPARENT { 0.0f, 0.0f, 0.0f, 0.0f };

// ---------------------------------------------------------------------------
// Layout constants (in pixels)
// ---------------------------------------------------------------------------
inline constexpr float SIDEBAR_W    = 56.0f;
inline constexpr float HEADER_H     = 64.0f;
inline constexpr float CONTENT_PAD  = 8.0f;
inline constexpr float CARD_ROUND   = 6.0f;
inline constexpr float CARD_PAD     = 8.0f;

// ---------------------------------------------------------------------------
// Style/color stack wrappers — thin shims over ImGui's Push/Pop APIs.
// Keep them in the header so unused-function warnings don't fire when only
// a subset is used by a given translation unit.
// ---------------------------------------------------------------------------
inline void pushStyleColor(ImGuiCol idx, Color col) {
    ImGui::PushStyleColor(idx, ImVec4(col.x, col.y, col.z, col.w));
}
inline void popStyleColor(int count = 1) { ImGui::PopStyleColor(count); }

inline void pushStyleVarFloat(ImGuiStyleVar idx, float v) {
    ImGui::PushStyleVar(idx, v);
}
inline void pushStyleVarVec2(ImGuiStyleVar idx, float x, float y) {
    ImGui::PushStyleVar(idx, ImVec2(x, y));
}
inline void popStyleVar(int count = 1) { ImGui::PopStyleVar(count); }

// ---------------------------------------------------------------------------
// applyModernTheme / popModernTheme — call after ImGui::StyleColorsDark().
// applyModernTheme pushes 14 style vars + 30 style colors; popModernTheme
// pops exactly that many. They MUST be balanced.
// ---------------------------------------------------------------------------
void applyModernTheme();
void popModernTheme();

// Number of style colors / vars pushed by applyModernTheme. Exposed so
// callers can verify balance in asserts if they want to pop manually.
inline constexpr int kPushedStyleColors = 30;
inline constexpr int kPushedStyleVars   = 14;

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
bool beginCard(const char* id, float w, float h, bool auto_resize_y);
bool beginCardWithFlags(const char* id, float w, float h,
                        bool auto_resize_y, ImGuiWindowFlags flags);
void endCard();

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
// Header logo — "ZZ" white + "CASTER" red, drawn on the header bar via
// ImDrawList. Call inside a Begin/End block.
// ---------------------------------------------------------------------------
void drawLogo(float at_x, float at_y);

// ---------------------------------------------------------------------------
// Text helpers — colored text via the theme palette. Overloads accept either
// a Color struct or one of the COL_* constants.
// ---------------------------------------------------------------------------
void textColored(Color col, const char* fmt, ...);
void textWrapped (Color col, const char* fmt, ...);

// ---------------------------------------------------------------------------
// Small inline helpers for layout
// ---------------------------------------------------------------------------
inline void hspace(float w) { ImGui::Dummy(ImVec2(w, 0)); }
inline void vspace(float h) { ImGui::Dummy(ImVec2(0, h)); }

// ---------------------------------------------------------------------------
// Internal color conversion — exposed because drawLogo uses it
// ---------------------------------------------------------------------------
inline std::uint32_t colorU32(Color col) {
    return ImGui::ColorConvertFloat4ToU32(ImVec4(col.x, col.y, col.z, col.w));
}

} // namespace caster::common::ui_theme
