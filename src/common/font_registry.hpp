// src/common/font_registry.hpp
//
// Global registry of ImGui fonts loaded at startup by GuiWindow.
//
// The theme system needs to know which ImFont* corresponds to each
// logical role so it can push the right font per active theme. We avoid
// stashing ImFont* inside the Theme struct itself (which lives in
// ui_theme.hpp, a header that doesn't include imgui.h) and instead expose
// them via this free-function registry.
//
// GuiWindow::GuiWindow() populates the registry after ImGui::CreateContext()
// succeeds. All other code reads via font_registry::xxx() at draw time.

#pragma once

struct ImFont;

namespace caster::common::font_registry {

// Must be called once after ImGui::CreateContext() and after the font
// atlas has been built. Stores raw pointers; the ImGuiContext owns the
// memory.
//
// `body`    — Inter Regular, body size (body text, labels, buttons)
// `body_sm` — Inter Regular, small size (uppercase micro-labels, field labels)
// `body_lg` — Inter Regular, large size (room code, display text)
// `mono`    — JetBrains Mono (keybinds, numeric values, room codes)
void set(ImFont* body, ImFont* body_sm, ImFont* body_lg, ImFont* mono);

// Accessors. Return nullptr if set() was never called (e.g. ImGui failed
// to init). Callers should fall back to ImGui's default font in that case.
ImFont* body();
ImFont* body_sm();
ImFont* body_lg();
ImFont* mono();

} // namespace caster::common::font_registry
