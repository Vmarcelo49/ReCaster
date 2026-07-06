// src/common/sdl_xinput_stub.cpp
//
// Stub for SDL_XINPUT_GetSteamVirtualGamepadSlot().
//
// SDL 2.30's SDL_windowsjoystick.c calls this function unconditionally
// from WINDOWS_JoystickGetDeviceSteamVirtualGamepadSlot(), but the
// implementation is only compiled when SDL_JOYSTICK_XINPUT is defined
// (which we leave OFF to avoid the XINPUT_CAPABILITIES_EX struct
// redefinition conflict between SDL_xinput.h and mingw-w64's xinput.h).
//
// We provide a no-op stub that returns -1 (no slot). This is fine for
// our use case — we don't use Steam Virtual Gamepad features.

extern "C" int SDL_XINPUT_GetSteamVirtualGamepadSlot(unsigned char userid) {
    (void)userid;
    return -1;
}
