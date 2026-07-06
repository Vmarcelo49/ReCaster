// Minimal SDL2/ImGui stubs for syntax-checking gui_window.cpp.
#pragma once

// ImGui shim — reuse from imgui_stub.h
#include "imgui.h"

// SDL2 shim
struct SDL_Window;
typedef struct SDL_GLContextState* SDL_GLContext;
struct SDLEventWindow { int event; int windowID; };
struct SDL_Event { int type; SDLEventWindow window; };
extern "C" {
    int SDL_Init(unsigned int);
    int SDL_InitSubSystem(unsigned int);
    void SDL_Quit();
    int SDL_WasInit(unsigned int);
    int SDL_GL_SetAttribute(int, int);
    SDL_Window* SDL_CreateWindow(const char*, int, int, int, int, unsigned int);
    void SDL_DestroyWindow(SDL_Window*);
    SDL_GLContext SDL_GL_CreateContext(SDL_Window*);
    void SDL_GL_DeleteContext(SDL_GLContext);
    int SDL_GL_MakeCurrent(SDL_Window*, SDL_GLContext);
    int SDL_GL_SetSwapInterval(int);
    void SDL_GL_SwapWindow(SDL_Window*);
    int SDL_PollEvent(SDL_Event*);
    int SDL_GetWindowID(SDL_Window*);
    void SDL_GetWindowSize(SDL_Window*, int*, int*);
    void SDL_SetWindowTitle(SDL_Window*, const char*);
    const char* SDL_GetError();
    const char* SDL_GetBasePath();
    int SDL_ShowSimpleMessageBox(unsigned int, const char*, const char*, SDL_Window*);
    int SDL_GetTicks();
}
#define SDL_INIT_VIDEO 1
#define SDL_INIT_TIMER 2
#define SDL_INIT_GAMECONTROLLER 4
#define SDL_INIT_JOYSTICK 8
#define SDL_WINDOW_OPENGL 1
#define SDL_WINDOW_SHOWN 2
#define SDL_WINDOWPOS_CENTERED 0
#define SDL_GL_RED_SIZE 1
#define SDL_GL_GREEN_SIZE 2
#define SDL_GL_BLUE_SIZE 3
#define SDL_GL_ALPHA_SIZE 4
#define SDL_GL_DOUBLEBUFFER 5
#define SDL_GL_DEPTH_SIZE 6
#define SDL_GL_STENCIL_SIZE 7
#define SDL_GL_CONTEXT_MAJOR_VERSION 8
#define SDL_GL_CONTEXT_MINOR_VERSION 9
#define SDL_GL_CONTEXT_PROFILE_MASK 10
#define SDL_GL_CONTEXT_PROFILE_CORE 1
#define SDL_GL_CONTEXT_PROFILE_COMPATIBILITY 2
#define SDL_MESSAGEBOX_ERROR 1
#define SDL_QUIT 0x100
#define SDL_WINDOWEVENT 0x200
#define SDL_WINDOWEVENT_CLOSE 1

// ImGui backend stubs
extern "C" {
    int ImGui_ImplSDL2_InitForOpenGL(SDL_Window*, SDL_GLContext);
    void ImGui_ImplSDL2_Shutdown();
    void ImGui_ImplSDL2_ProcessEvent(const SDL_Event*);
    void ImGui_ImplSDL2_NewFrame();
    int ImGui_ImplOpenGL3_Init(const char*);
    void ImGui_ImplOpenGL3_Shutdown();
    void ImGui_ImplOpenGL3_NewFrame();
    void ImGui_ImplOpenGL3_RenderDrawData(const void*);
}
#define IMGUI_CHECKVERSION() ((void)0)

// OpenGL shim
extern "C" {
    void glViewport(int, int, int, int);
    void glClearColor(float, float, float, float);
    void glClear(int);
}
#define GL_COLOR_BUFFER_BIT 1
#define GL_DEPTH_BUFFER_BIT 2
