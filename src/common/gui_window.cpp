// src/common/gui_window.cpp

#include "gui_window.hpp"
#include "embedded_font.hpp"
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
        SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN);
    if (!window_) return false;

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
    Uint32 sdl_flags = SDL_INIT_VIDEO | SDL_INIT_TIMER;
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

    // Load the embedded font (font.ttf copied from zzcaster, 18 px).
    // AddFontFromMemoryTTF wants a non-const pointer (it may rearrange the
    // data for stb_truetype). Since our font data is in a `static const`
    // array, ImGui will make an internal copy — passing the const pointer
    // is safe; the cast just satisfies the API signature.
    ImFont* font = io.Fonts->AddFontFromMemoryTTF(
        const_cast<std::uint8_t*>(embedded_font::data),
        static_cast<int>(embedded_font::size),
        18.0f);
    if (font) {
        io.FontDefault = font;
    } else {
        logger::warn("AddFontFromMemoryTTF failed; using ImGui default font");
    }

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

    // Configure the global ImGui style (colors, rounding, spacing) directly
    // on the Style struct — NOT via Push/Pop, which would leak the stack.
    // Pages that want a temporary override can use PushStyleColor themselves.
    // We start from StyleColorsDark and then overlay our palette on top.
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();

    // Geometry
    style.WindowPadding    = ImVec2(0, 0);
    style.WindowRounding   = 0.0f;
    style.WindowBorderSize = 0.0f;
    style.ChildRounding    = ui_theme::CARD_ROUND;
    style.ChildBorderSize  = 1.0f;
    style.FramePadding     = ImVec2(8, 5);
    style.FrameRounding    = 6.0f;
    style.FrameBorderSize  = 0.0f;
    style.ItemSpacing      = ImVec2(8, 8);
    style.ItemInnerSpacing = ImVec2(6, 4);
    style.IndentSpacing    = 16.0f;
    style.ScrollbarRounding = 8.0f;
    style.GrabRounding     = 4.0f;
    style.TabRounding      = 4.0f;

    // Palette
    auto to_imvec4 = [](const ui_theme::Color& c) {
        return ImVec4(c.x, c.y, c.z, c.w);
    };
    auto& cols = style.Colors;
    cols[ImGuiCol_WindowBg]             = to_imvec4(ui_theme::COL_TRANSPARENT);
    cols[ImGuiCol_ChildBg]              = to_imvec4(ui_theme::COL_CARD);
    cols[ImGuiCol_Border]               = to_imvec4(ui_theme::COL_CARD_BRD);
    cols[ImGuiCol_Text]                 = to_imvec4(ui_theme::COL_TEXT);
    cols[ImGuiCol_TextDisabled]         = to_imvec4(ui_theme::COL_MUTED);
    cols[ImGuiCol_TextLink]             = to_imvec4(ui_theme::COL_RED);
    cols[ImGuiCol_FrameBg]              = to_imvec4(ui_theme::COL_FRAME);
    cols[ImGuiCol_FrameBgHovered]       = to_imvec4(ui_theme::COL_FRAME_HOV);
    cols[ImGuiCol_FrameBgActive]        = to_imvec4(ui_theme::COL_FRAME_ACT);
    cols[ImGuiCol_Button]               = to_imvec4(ui_theme::COL_RED);
    cols[ImGuiCol_ButtonHovered]        = to_imvec4(ui_theme::COL_RED_HOV);
    cols[ImGuiCol_ButtonActive]         = to_imvec4(ui_theme::COL_RED_ACT);
    cols[ImGuiCol_Header]               = to_imvec4(ui_theme::COL_NAV_ACTIVE);
    cols[ImGuiCol_HeaderHovered]        = to_imvec4(ui_theme::COL_NAV_HOV);
    cols[ImGuiCol_HeaderActive]         = to_imvec4(ui_theme::COL_NAV_ACTIVE);
    cols[ImGuiCol_CheckMark]            = to_imvec4(ui_theme::COL_RED);
    cols[ImGuiCol_Separator]            = to_imvec4(ui_theme::COL_CARD_BRD);
    cols[ImGuiCol_SeparatorHovered]     = to_imvec4(ui_theme::COL_RED_DIM);
    cols[ImGuiCol_SeparatorActive]      = to_imvec4(ui_theme::COL_RED);
    cols[ImGuiCol_ScrollbarBg]          = to_imvec4(ui_theme::COL_TRANSPARENT);
    cols[ImGuiCol_ScrollbarGrab]        = to_imvec4(ui_theme::COL_FRAME);
    cols[ImGuiCol_ScrollbarGrabHovered] = to_imvec4(ui_theme::COL_FRAME_HOV);
    cols[ImGuiCol_ScrollbarGrabActive]  = to_imvec4(ui_theme::COL_FRAME_ACT);
    cols[ImGuiCol_SliderGrab]           = to_imvec4(ui_theme::COL_RED);
    cols[ImGuiCol_SliderGrabActive]     = to_imvec4(ui_theme::COL_RED_HOV);
    cols[ImGuiCol_NavCursor]          = to_imvec4(ui_theme::COL_RED);
    cols[ImGuiCol_Tab]                  = to_imvec4(ui_theme::COL_FRAME);
    cols[ImGuiCol_TabHovered]           = to_imvec4(ui_theme::COL_RED_DIM);
    cols[ImGuiCol_TabSelected]          = to_imvec4(ui_theme::COL_RED);
    cols[ImGuiCol_TabSelectedOverline]  = to_imvec4(ui_theme::COL_RED_HOV);

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
    // Clear color matches the bottom of the gradient (COL_BG_MID) so any
    // region not covered by the gradient is visually consistent.
    glClearColor(0.16f, 0.16f, 0.18f, 1.00f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

    SDL_GL_SwapWindow(window_);
    return true;
}

} // namespace caster::common
