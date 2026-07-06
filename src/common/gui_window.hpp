// src/common/gui_window.hpp
//
// Shared SDL2 + Dear ImGui (OpenGL3 backend) window helper used by both
// caster.exe (the launcher) and hook.dll (the injected payload).
//
// Both binaries need to spawn an SDL2 window with an ImGui context bound to
// an OpenGL3 renderer. The differences are minimal, so we factor the boiler-
// plate into a single RAII class.
//
// Threading contract:
//   - One GuiWindow per thread. SDL2 contexts are not safe to share across
//     threads without explicit gl_make_current calls.
//   - The thread that constructs a GuiWindow is the thread that must call
//     pump_frame() on it for the lifetime of the window.
//
// Lifecycle:
//   GuiWindow w("title", 1024, 768);
//   while (w.pump_frame([&]{ ImGui::Begin("..."); ...; ImGui::End(); })) {
//       // frame rendered; loop continues until user closes window
//   }
//   // w destroyed here, SDL2/ImGui torn down cleanly
//
// Construction failure: SDL/GL init failures are reported via
// SDL_ShowSimpleMessageBox (not exceptions) so the user sees a friendly
// dialog even when the GL context can't be created. Use is_open() after
// construction to check success — if false, the window is unusable and
// should be destroyed.

#pragma once

#include <functional>
#include <string>

// Forward declarations to keep SDL2/ImGui headers out of the public API
// of this header (consumers don't always need to include them).
// Note: SDL_GLContext is `void*` in real SDL2; we declare it the same way
// to avoid a typedef conflict when consumers later include SDL.h.
struct SDL_Window;
struct ImGuiContext;
struct ImFont;
using SDL_GLContext = void*;

namespace caster::common {

class GuiWindow {
public:
    GuiWindow(const char* title, int width, int height);
    ~GuiWindow();

    GuiWindow(const GuiWindow&)            = delete;
    GuiWindow& operator=(const GuiWindow&) = delete;
    GuiWindow(GuiWindow&&)                 = delete;
    GuiWindow& operator=(GuiWindow&&)      = delete;

    // Pump SDL events, start a new ImGui frame, invoke `draw`, render, present.
    // Returns true to continue looping; false when the window has been
    // requested to close (SDL_QUIT or window-close button) or when the
    // window failed to initialize.
    bool pump_frame(std::function<void()> draw);

    // Force-close the window (next pump_frame will return false).
    void request_close() { open_ = false; }

    // True between successful construction and the first pump_frame that
    // returns false. **Also false if construction failed** — caller MUST
    // check this before entering the loop.
    bool is_open() const { return open_; }

    // GLSL version string passed to ImGui_ImplOpenGL3_Init.
    // "#version 130" for GL 3.0 core, "#version 110" for GL 2.1 compat.
    const char* glsl_version() const { return glsl_version_.c_str(); }

    SDL_Window* sdl_window() const { return window_; }

private:
    SDL_Window*    window_       = nullptr;
    SDL_GLContext  gl_context_   = nullptr;
    ImGuiContext*  imgui_ctx_    = nullptr;
    bool           open_         = false;
    bool           sdl_initialized_by_us_ = false;
    std::string    glsl_version_ = "#version 130";

    // Try to create the SDL window + GL context with the given GL version
    // and profile. Returns true on success (window_ + gl_context_ filled
    // in), false on failure (window_ destroyed if partial).
    bool tryCreateWithContext(int major, int minor, int profile_mask);
};

// Helper: call SDL_Quit() once at process exit. Safe to call from both
// caster.exe (main) and hook.dll (worker thread before thread exit).
// Idempotent; the first caller wins, subsequent calls are no-ops.
void maybe_shutdown_sdl();

} // namespace caster::common
