// src/common/ui_theme.hpp
//
// Themeable UI primitives for the caster launcher.
//
// What this module owns:
//   - Theme struct: palette + typography pointers + geometry knobs
//   - Three built-in themes: Default (dark red & black), Modern (cyan/indigo),
//     Elegant (warm bronze serif).
//   - Active theme getter/setter (set_active_theme reads from Config)
//   - Helper drawing primitives (navTab, toggle, segmentedControl, etc.)
//   - Legacy color constants (COL_RED, COL_BG_*) kept for transitional
//     compatibility with code that hasn't been ported yet.
//
// What this module does NOT do: anything game-, netplay-, or session-related.
// Logic lives in src/exe/pages/.
//
// Font role mapping:
//   body    — Inter Regular 16px (default; body text, buttons, inputs)
//   body_sm — Inter Regular 13px (uppercase micro-labels, field labels)
//   display — Fraunces Italic 22px (player numbers, big titles in Elegant)
//   mono    — JetBrains Mono 13px (keybinds, values, room codes)
//
// Theme → font role mapping (which font is used for which role):
//   Default: body=body, display=body (just larger), mono=mono
//   Modern:  body=body, display=body (just larger), mono=mono
//   Elegant: body=body, display=display (Fraunces italic), mono=mono
//
// Pushing fonts is the caller's responsibility via PushFont/PopFont — we
// just expose the right pointers via the Theme struct.

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
// ThemeId — small enum stored in Config as int (0/1/2).
// ---------------------------------------------------------------------------
enum class ThemeId : std::uint8_t {
    Default = 0,
    Modern  = 1,
    Elegant = 2,
};

// Convert int (from config) → ThemeId. Clamps to valid range.
ThemeId theme_id_from_int(int v);

// Slider visual style per theme.
enum class SliderStyle : std::uint8_t {
    Thin,    // Elegant: 1px line + small dot
    Thick,   // Default: 4px bar with gradient fill
    Modern,  // Modern: 4px bar with glow thumb
};

// Toggle knob style per theme.
enum class ToggleStyle : std::uint8_t {
    Square,   // Default: sharp corners
    Pill,     // Modern + Elegant: 999px radius (pill shape)
};

// Input visual style per theme.
enum class InputStyle : std::uint8_t {
    Box,        // Default: rectangular bordered box
    Rounded,    // Modern: 6px rounded box with focus glow
    Underline,  // Elegant: bottom underline only, transparent bg
};

// Button visual style per theme.
enum class ButtonStyle : std::uint8_t {
    Sharp,    // Default: 0 radius, bordered
    Rounded,  // Modern: 6px radius
    Flat,     // Elegant: 2px radius, transparent bg, inverts on hover
};

// ---------------------------------------------------------------------------
// Theme — all visual identity parameters for one look-and-feel.
//
// Lifetime: Theme instances are static const globals defined in ui_theme.cpp.
// They live for the entire program. ImFont* fields are filled lazily on
// first access via font_registry (which is set up by GuiWindow).
// ---------------------------------------------------------------------------
struct Theme {
    ThemeId     id;
    const char* name;          // "Red", "Blue", "Elegant Summer"

    // Backgrounds (vertical gradient: top → bottom)
    Color bg_top;
    Color bg_bottom;
    Color bg_elevated;         // header bar, secondary surfaces
    Color bg_card;             // not used much in the new layout, kept for compat

    // Rules / borders at three intensities
    Color rule;
    Color rule_mid;
    Color rule_strong;

    // Text colors
    Color text;
    Color text_muted;
    Color text_dim;

    // Accent (primary brand color)
    Color accent;
    Color accent_soft;         // darker variant
    Color accent_2;            // secondary accent (Modern only); equals accent otherwise
    Color accent_bg;           // translucent accent for hovers/focus rings

    // Status colors (same across themes, but exposed for customization)
    Color error;
    Color success;
    Color warn;
    Color info;

    // Geometry
    float input_radius;
    float btn_radius;
    float toggle_radius;

    // Behavior flags
    SliderStyle  slider_style;
    ToggleStyle  toggle_style;
    InputStyle   input_style;
    ButtonStyle  button_style;

    // Typography role mapping.
    // Each role returns the ImFont* to PushFont() for that role.
    // Returns nullptr if no font has been registered yet (caller should
    // fall back to ImGui's default font).
    ImFont* font_body() const;     // body text, buttons, inputs, titles
    ImFont* font_body_sm() const;  // micro-labels, hints
    ImFont* font_body_lg() const;  // large display text (room code)
    ImFont* font_mono() const;     // keybinds, values, room codes
};

// ---------------------------------------------------------------------------
// Active theme management.
//
// set_active_theme() updates the active theme AND reconfigures the global
// ImGui style (ImGuiStyle::Colors[], rounding, padding) so subsequent
// ImGui::* calls pick up the new look without per-frame PushStyleVar.
// Call this once at startup with the value from Config, and again whenever
// the user changes the theme in the Config page.
// ---------------------------------------------------------------------------
const Theme& active_theme();
void         set_active_theme(ThemeId id);
ThemeId      active_theme_id();

// Global override: force all corner radii to 0 (sharp) when false, or
// use the theme's native radii when true. Defaults to false (sharp).
// Persisted in Config as [ui]/rounded_corners. Affects all themes.
void set_rounded_corners(bool enabled);
bool rounded_corners_enabled();

// Apply the active theme's palette to the global ImGuiStyle. Called by
// set_active_theme() and at startup. Idempotent.
void apply_theme_to_imgui_style();

// ---------------------------------------------------------------------------
// Layout constants (in pixels) — shared by all themes.
// These match the HTML reference layout.
// ---------------------------------------------------------------------------
inline constexpr float WINDOW_W       = 1024.0f;  // default/initial size
inline constexpr float WINDOW_H       = 768.0f;   // default/initial size
inline constexpr float HEADER_H       = 72.0f;     // was 64; bumped to match HTML
inline constexpr float CONTENT_PAD_Y  = 40.0f;     // top/bottom padding inside content area
inline constexpr float CONTENT_PAD_X  = 48.0f;     // left/right padding inside content area
inline constexpr float CARD_ROUND     = 6.0f;
inline constexpr float CARD_PAD       = 8.0f;
inline constexpr float NAV_BUTTON_PAD = 7.0f;

// ---------------------------------------------------------------------------
// Font sizes (in pixels) — all font sizes live here so there are no magic
// numbers scattered across the codebase. To globally scale fonts, change
// these values and everything updates.
//
// Current values are baseline + 3px (first global bump).
// ---------------------------------------------------------------------------
inline constexpr float FONT_SIZE_BODY    = 19.0f;  // body text, buttons, inputs
inline constexpr float FONT_SIZE_BODY_SM = 16.0f;  // uppercase micro-labels, field labels
inline constexpr float FONT_SIZE_BODY_LG = 38.0f;  // large display text (room code)
inline constexpr float FONT_SIZE_MONO    = 16.0f;  // keybinds, numeric values, room codes

// Minimum window size — enforced by SDL_SetWindowMinimumSize. The UI is
// designed to be responsive above this size but will look cramped below it.
inline constexpr float WINDOW_MIN_W   = 800.0f;
inline constexpr float WINDOW_MIN_H   = 600.0f;

// Helpers: actual window dimensions for the current frame. These query
// ImGui's main viewport (which tracks the SDL window size) so the layout
// adapts when the user resizes the window. Falls back to the default
// constants if the viewport isn't available yet (first frame).
float window_width();
float window_height();

// Legacy sidebar width — kept for transitional code; not used by new layout.
inline constexpr float SIDEBAR_W      = 0.0f;

// ---------------------------------------------------------------------------
// Style/color stack wrappers — thin shims over ImGui's Push/Pop APIs.
// ---------------------------------------------------------------------------
inline void pushStyleColor(ImGuiCol idx, Color col) {
    ImGui::PushStyleColor(idx, ImVec4(col.x, col.y, col.z, col.w));
}
inline void popStyleColor(int count = 1) { ImGui::PopStyleColor(count); }

// ---------------------------------------------------------------------------
// drawGradientBackground — full-window vertical gradient via ImDrawList.
// Uses the active theme's bg_top → bg_bottom. Call this INSIDE a Begin/End
// block (uses the window's draw list).
// ---------------------------------------------------------------------------
void drawGradientBackground();

// ---------------------------------------------------------------------------
// Card container — child window with consistent bg/border/rounding.
// Kept for transitional compatibility with not-yet-ported pages.
// ---------------------------------------------------------------------------
bool beginCard(const char* id, float w, float h, bool auto_resize_y,
               ImGuiWindowFlags flags = 0);
void endCard();
bool beginCenteredCard(const char* id, float w, float h,
                       bool auto_resize_y = false,
                       ImGuiWindowFlags flags = 0);
void cardTitle(const char* text);

// ---------------------------------------------------------------------------
// NEW: Navigation tab (header) — uppercase text button with animated
// underline. `active` controls whether the underline is fully drawn.
// Returns true if clicked this frame.
// ---------------------------------------------------------------------------
bool navTab(const char* label, bool active);

// ---------------------------------------------------------------------------
// NEW: Segmented control — N mutually-exclusive buttons in a row.
// `active_index` is read AND written (set on click). Returns true if the
// active index changed this frame.
// ---------------------------------------------------------------------------
bool segmentedControl(const char* const* labels, int count, int* active_index);

// ---------------------------------------------------------------------------
// NEW: Toggle switch — iOS-style pill toggle. `value` is read AND written.
// `id` is used as the ImGui ID (must be unique within the parent window).
// Returns true if the value changed this frame.
// ---------------------------------------------------------------------------
bool toggle(bool* value, const char* id = "##toggle");

// ---------------------------------------------------------------------------
// NEW: Themed slider for float values. Wraps ImGui::SliderFloat but draws
// a custom track + grab via ImDrawList so the visual matches the HTML
// reference (thin/thick/modern variants per theme).
// Returns true if the value changed this frame.
// ---------------------------------------------------------------------------
bool themedSliderFloat(const char* label, float* v, float v_min, float v_max,
                       const char* format = "%.2f");

// ---------------------------------------------------------------------------
// NEW: Keybind button — monospace button for controller bindings.
// `listening` makes the button render in the accent color (the user has
// clicked it and we're waiting for an input event).
// Returns true if clicked this frame.
// ---------------------------------------------------------------------------
bool keybindButton(const char* label, bool listening, float width = 72.0f);

// ---------------------------------------------------------------------------
// NEW: Action button (HTML .action-btn equivalent). Three variants:
//   default — bordered box, hover inverts to accent
//   primary — accent-filled, hover darkens
//   ghost   — transparent, hover underline-only
// ---------------------------------------------------------------------------
enum class ButtonVariant { Default, Primary, Ghost };
bool actionButton(const char* label, float w, float h,
                  ButtonVariant variant = ButtonVariant::Default);

// Convenience wrappers (keep the old primaryButton/secondaryButton names
// as thin shims so unported pages still compile).
inline bool primaryButton(const char* label, float w, float h) {
    return actionButton(label, w, h, ButtonVariant::Primary);
}
inline bool secondaryButton(const char* label, float w, float h) {
    return actionButton(label, w, h, ButtonVariant::Default);
}

// ---------------------------------------------------------------------------
// NEW: Field label — small uppercase muted text used above inputs/lists.
// Renders at body_sm size with letter-spacing emulation via uppercase.
// ---------------------------------------------------------------------------
void fieldLabel(const char* text);

// ---------------------------------------------------------------------------
// NEW: Vertical separator — draws a 1px vertical line from (x, y0) to
// (x, y1) using rule color. Use between two columns of content.
// ---------------------------------------------------------------------------
void verticalSeparator(float x, float y0, float y1);

// ---------------------------------------------------------------------------
// NEW: Brand block — draws the "NETPLAY CLIENT" kicker + "RE CASTER"
// name at (x, y) using the active theme's display font (Elegant) or body
// font (Default/Modern). Matches the HTML .brand structure.
// ---------------------------------------------------------------------------
void drawBrand(float at_x, float at_y);

// ---------------------------------------------------------------------------
// Legacy nav button (sidebar) — kept for transitional compat.
// The new layout doesn't use a sidebar; this is here so unported code
// still compiles.
// ---------------------------------------------------------------------------
bool navButton(const char* letter, bool active);

// ---------------------------------------------------------------------------
// Header logo (legacy) — redirects to drawBrand. Kept for compat.
// ---------------------------------------------------------------------------
void drawLogo(float at_x, float at_y);

// ---------------------------------------------------------------------------
// Colored text helpers — push color, draw text, pop color.
// ---------------------------------------------------------------------------
void drawErrorText(const char* fmt, ...);
void drawSuccessText(const char* fmt, ...);
void drawWarnText(const char* fmt, ...);
void drawInfoText(const char* fmt, ...);

// ---------------------------------------------------------------------------
// Legacy color constants — kept for unported code. They alias the Elegant
// theme's palette (which is closest to the original dark/red look).
// New code should use active_theme().accent etc. directly.
// ---------------------------------------------------------------------------
inline constexpr Color COL_RED     { 0.75f, 0.22f, 0.17f, 1.00f };
inline constexpr Color COL_RED_HOV { 0.66f, 0.18f, 0.15f, 1.00f };
inline constexpr Color COL_RED_ACT { 0.55f, 0.14f, 0.12f, 1.00f };
inline constexpr Color COL_RED_DIM { 0.45f, 0.13f, 0.10f, 1.00f };

inline constexpr Color COL_BG_DARK { 0.10f, 0.10f, 0.11f, 1.00f };
inline constexpr Color COL_BG_MID  { 0.16f, 0.16f, 0.18f, 1.00f };

inline constexpr Color COL_CARD     { 0.10f, 0.10f, 0.11f, 0.92f };
inline constexpr Color COL_CARD_BRD { 0.27f, 0.27f, 0.30f, 1.00f };

inline constexpr Color COL_SIDEBAR { 0.07f, 0.07f, 0.08f, 0.95f };

inline constexpr Color COL_FRAME     { 0.18f, 0.18f, 0.20f, 1.00f };
inline constexpr Color COL_FRAME_HOV { 0.22f, 0.22f, 0.25f, 1.00f };
inline constexpr Color COL_FRAME_ACT { 0.26f, 0.26f, 0.30f, 1.00f };

inline constexpr Color COL_HEADER_BAR { 0.05f, 0.05f, 0.06f, 0.95f };

inline constexpr Color COL_TEXT     { 0.92f, 0.92f, 0.94f, 1.00f };
inline constexpr Color COL_MUTED    { 0.50f, 0.50f, 0.55f, 1.00f };

inline constexpr Color COL_NAV        { 0.00f, 0.00f, 0.00f, 0.00f };
inline constexpr Color COL_NAV_HOV    { 0.20f, 0.20f, 0.22f, 0.60f };
inline constexpr Color COL_NAV_ACTIVE { 0.30f, 0.10f, 0.08f, 0.50f };

inline constexpr Color COL_TRANSPARENT { 0.0f, 0.0f, 0.0f, 0.0f };

inline constexpr Color COL_ERROR   { 1.00f, 0.40f, 0.40f, 1.00f };
inline constexpr Color COL_SUCCESS { 0.40f, 0.90f, 0.40f, 1.00f };
inline constexpr Color COL_WARN    { 1.00f, 0.60f, 0.20f, 1.00f };
inline constexpr Color COL_INFO    { 1.00f, 0.85f, 0.20f, 1.00f };

} // namespace caster::common::ui_theme
