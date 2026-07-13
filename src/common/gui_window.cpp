// src/common/gui_window.cpp

#include "gui_window.hpp"
#include "embedded_font.hpp"
#include "font_registry.hpp"
#include "logger.hpp"
#include "ui_theme.hpp"

#include <SDL2/SDL.h>
#include <SDL2/SDL_opengl.h>

#include <imgui.h>
#include <imgui_impl_sdl2.h>
#include <imgui_impl_opengl3.h>

#include <atomic>
#include <stdexcept>
#include <string>

namespace caster::common {

namespace {

// SDL_Quit() must be called exactly once per process. We track that with
// an atomic flag. Both caster.exe and hook.dll may end up calling this —
// the first one wins, the rest are no-ops.
std::atomic<bool> g_sdl_quit_called{false};

// Show a friendly error dialog. Falls back to stderr if SDL isn't ready.
void showErrorBox(const char* title, const char* msg) {
    if (SDL_WasInit(SDL_INIT_VIDEO)) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, title, msg, nullptr);
    } else {
        std::fprintf(stderr, "[%s] %s\n", title, msg);
    }
    logger::err("GuiWindow: {}: {}", title, msg);
}

} // namespace

void maybe_shutdown_sdl() {
    bool expected = false;
    if (g_sdl_quit_called.compare_exchange_strong(expected, true)) {
        SDL_Quit();
    }
}

bool GuiWindow::tryCreateWithContext(int major, int minor, int profile_mask) {
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, major);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, minor);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,  profile_mask);

    // Note: caller has already set RED/GREEN/BLUE/ALPHA/DEPTH/STENCIL/DOUBLEBUFFER.

    window_ = SDL_CreateWindow(
        "caster",  // title is set later by caller; SDL_CreateWindow needs *something* here
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        1024, 768,
        SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
    if (!window_) return false;

    // Set a minimum window size so the UI doesn't collapse into an
    // unusable state. 800x600 is comfortable for the 3-tab layout.
    SDL_SetWindowMinimumSize(window_, 800, 600);

    gl_context_ = SDL_GL_CreateContext(window_);
    if (!gl_context_) {
        SDL_DestroyWindow(window_);
        window_ = nullptr;
        return false;
    }
    if (SDL_GL_MakeCurrent(window_, gl_context_) < 0) {
        SDL_GL_DeleteContext(gl_context_);
        gl_context_ = nullptr;
        SDL_DestroyWindow(window_);
        window_ = nullptr;
        return false;
    }
    return true;
}

GuiWindow::GuiWindow(const char* title, int width, int height) {
    // SDL_Init is reference-counted in 2.x; calling it more than once is fine
    // as long as SDL_Quit is called the same number of times.
    // Joystick subsystem is needed for the controller mapping UI.
    Uint32 sdl_flags = SDL_INIT_VIDEO | SDL_INIT_TIMER | SDL_INIT_JOYSTICK;
    if (SDL_Init(sdl_flags) < 0) {
        showErrorBox("caster — SDL init failed", SDL_GetError());
        return;
    }
    sdl_initialized_by_us_ = true;

    // Request double-buffered 24/8 depth/stencil + 8-bit RGBA. These are
    // the same attributes zzcaster requests.
    SDL_GL_SetAttribute(SDL_GL_RED_SIZE,   8);
    SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE,  8);
    SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);

    // ---- Attempt 1: OpenGL 3.0 core profile -----------------------------
    // Works on Win7+ and any GPU from the last ~10 years. Maps to
    // imgui_impl_opengl3 with GLSL #version 130.
    if (tryCreateWithContext(3, 0, SDL_GL_CONTEXT_PROFILE_CORE)) {
        glsl_version_ = "#version 130";
    } else {
        logger::warn("GL 3.0 core failed: {}; trying GL 2.1 compatibility",
                     SDL_GetError());

        // ---- Attempt 2: OpenGL 2.1 compatibility profile ----------------
        // Fallback for ancient Intel HD 4000 / Microsoft Basic Display
        // Adapter / some VMs. Maps to imgui_impl_opengl3 with GLSL #version 110.
        if (tryCreateWithContext(2, 1, SDL_GL_CONTEXT_PROFILE_COMPATIBILITY)) {
            glsl_version_ = "#version 110";
            logger::info("Falling back to OpenGL 2.1 compatibility (GLSL {})",
                         glsl_version_);
        } else {
            // Both attempts failed — show a friendly dialog and bail.
            std::string msg =
                "Could not create an OpenGL context.\n\n"
                "Attempted:\n"
                "  - OpenGL 3.0 core profile (GLSL #version 130)\n"
                "  - OpenGL 2.1 compatibility (GLSL #version 110)\n\n"
                "Last error: ";
            msg += SDL_GetError();
            msg += "\n\nTroubleshooting:\n"
                   "  - Update your GPU drivers.\n"
                   "  - On Windows: enable 'Hardware acceleration' in your GPU control panel.\n"
                   "  - On VMs: enable 3D acceleration in the hypervisor.\n"
                   "  - Under Wine: install DXVK or use a native OpenGL driver.";
            showErrorBox("caster — OpenGL init failed", msg.c_str());
            return;
        }
    }

    // Now we have a working window + GL context. Set the title properly.
    SDL_SetWindowTitle(window_, title);

    // Enable vsync if the driver supports it; non-fatal if it doesn't.
    if (SDL_GL_SetSwapInterval(1) < 0) {
        logger::warn("VSync not supported: {}", SDL_GetError());
    }

    // ---- ImGui ----------------------------------------------------------
    IMGUI_CHECKVERSION();
    imgui_ctx_ = ImGui::CreateContext();
    if (!imgui_ctx_) {
        showErrorBox("caster — ImGui init failed",
                     "ImGui::CreateContext returned null");
        SDL_GL_DeleteContext(gl_context_);
        gl_context_ = nullptr;
        SDL_DestroyWindow(window_);
        window_ = nullptr;
        return;
    }
    ImGui::StyleColorsDark();

    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename  = nullptr;  // don't write imgui.ini next to the binary

    // Load the embedded fonts at the sizes defined in ui_theme.hpp
    // (FONT_SIZE_BODY / FONT_SIZE_BODY_SM / FONT_SIZE_MONO) and register
    // them so the theme system can PushFont() per role.
    //
    // AddFontFromMemoryTTF wants a non-const pointer (it may rearrange the
    // data for stb_truetype). Our font data lives in `static const` arrays,
    // so ImGui makes an internal copy — passing the const pointer is safe;
    // the cast just satisfies the API signature.
    auto load_font = [&io](const std::uint8_t* data, std::size_t size,
                        float px) -> ImFont* {
        return io.Fonts->AddFontFromMemoryTTF(
            const_cast<std::uint8_t*>(data),
            static_cast<int>(size),
            px);
    };

    ImFont* f_body    = load_font(embedded_font::interRegular::data,
                                  embedded_font::interRegular::size,
                                  ui_theme::FONT_SIZE_BODY);
    ImFont* f_body_sm = load_font(embedded_font::interRegular::data,
                                  embedded_font::interRegular::size,
                                  ui_theme::FONT_SIZE_BODY_SM);
    ImFont* f_body_lg = load_font(embedded_font::interRegular::data,
                                  embedded_font::interRegular::size,
                                  ui_theme::FONT_SIZE_BODY_LG);
    ImFont* f_mono    = load_font(embedded_font::jetbrainsMono::data,
                                  embedded_font::jetbrainsMono::size,
                                  ui_theme::FONT_SIZE_MONO);

    if (f_body) {
        io.FontDefault = f_body;
    } else {
        logger::warn("Failed to load Inter Regular body font; using ImGui default");
    }
    if (!f_body_sm) logger::warn("Failed to load Inter Regular small font");
    if (!f_body_lg) logger::warn("Failed to load Inter Regular large font");
    if (!f_mono)    logger::warn("Failed to load JetBrains Mono font");

    font_registry::set(f_body, f_body_sm, f_body_lg, f_mono);

    if (!ImGui_ImplSDL2_InitForOpenGL(window_, gl_context_)) {
        showErrorBox("caster — ImGui SDL2 backend init failed",
                     "ImGui_ImplSDL2_InitForOpenGL returned false");
        ImGui::DestroyContext(imgui_ctx_);
        imgui_ctx_ = nullptr;
        SDL_GL_DeleteContext(gl_context_);
        gl_context_ = nullptr;
        SDL_DestroyWindow(window_);
        window_ = nullptr;
        return;
    }
    if (!ImGui_ImplOpenGL3_Init(glsl_version_.c_str())) {
        showErrorBox("caster — ImGui OpenGL3 backend init failed",
                     "ImGui_ImplOpenGL3_Init returned false");
        ImGui_ImplSDL2_Shutdown();
        ImGui::DestroyContext(imgui_ctx_);
        imgui_ctx_ = nullptr;
        SDL_GL_DeleteContext(gl_context_);
        gl_context_ = nullptr;
        SDL_DestroyWindow(window_);
        window_ = nullptr;
        return;
    }

    // Configure the global ImGui style (colors, rounding, spacing) from
    // the active theme. set_active_theme() was already called from main()
    // with the persisted value from Config; apply_theme_to_imgui_style()
    // pushes that theme's palette + geometry into ImGuiStyle.
    ui_theme::apply_theme_to_imgui_style();

    open_ = true;
    logger::info("GuiWindow ready: {}x{} GLSL='{}'",
                 width, height, glsl_version_);
}

GuiWindow::~GuiWindow() {
    // ImGui teardown must happen while the GL context is current.
    if (gl_context_) {
        SDL_GL_MakeCurrent(window_, gl_context_);
    }
    if (imgui_ctx_) {
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplSDL2_Shutdown();
        ImGui::DestroyContext(imgui_ctx_);
        imgui_ctx_ = nullptr;
    }
    if (gl_context_) {
        SDL_GL_DeleteContext(gl_context_);
        gl_context_ = nullptr;
    }
    if (window_) {
        SDL_DestroyWindow(window_);
        window_ = nullptr;
    }
    // NOTE: we intentionally do NOT call SDL_Quit() here. SDL uses refcounted
    // subsystems and a process may have multiple GuiWindow instances
    // (e.g. caster.exe opening its window and hook.dll opening its own).
    // Callers should call maybe_shutdown_sdl() at the very end of the
    // process / worker thread.
}

bool GuiWindow::pump_frame(std::function<void()> draw) {
    if (!open_) return false;

    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        ImGui_ImplSDL2_ProcessEvent(&ev);
        switch (ev.type) {
            case SDL_QUIT:
                open_ = false;
                break;
            case SDL_WINDOWEVENT:
                if (ev.window.event == SDL_WINDOWEVENT_CLOSE &&
                    ev.window.windowID == SDL_GetWindowID(window_)) {
                    open_ = false;
                }
                break;
            default:
                break;
        }
    }
    if (!open_) return false;

    SDL_GL_MakeCurrent(window_, gl_context_);

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplSDL2_NewFrame();
    ImGui::NewFrame();

    if (draw) draw();

    ImGui::Render();

    int w = 0, h = 0;
    SDL_GetWindowSize(window_, &w, &h);
    glViewport(0, 0, w, h);
    // Clear color matches the bottom of the active theme's gradient so any
    // region not covered by the gradient (e.g. during resize) is visually
    // consistent with the background.
    const auto& bg = ui_theme::active_theme().bg_bottom;
    glClearColor(bg.x, bg.y, bg.z, bg.w);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

    SDL_GL_SwapWindow(window_);

    // ---- Frame rate cap (60 FPS) ----------------------------------------
    // VSync already limits to the monitor's refresh rate when available,
    // but on Wine/VMs without VSync, or on 144/240 Hz monitors, the launcher
    // would spin needlessly. We cap to 60 FPS via SDL_Delay — works in
    // addition to VSync (whichever kicks in first).
    constexpr std::uint32_t kTargetFrameMs = 1000 / 60;  // ~16ms
    const std::uint32_t now_ticks = SDL_GetTicks();
    if (last_frame_ticks_ != 0) {
        const std::uint32_t elapsed = now_ticks - last_frame_ticks_;
        if (elapsed < kTargetFrameMs) {
            SDL_Delay(kTargetFrameMs - elapsed);
        }
    }
    last_frame_ticks_ = SDL_GetTicks();

    return true;
}

} // namespace caster::common
