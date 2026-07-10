// src/dll/dll_hacks.cpp
// Ported from CCCaster DllHacks.cpp. Stripped: overlay init, keyboard/mouse managers.
// Kept: ASM patch lifecycle, WindowProc hook via MinHook, DX9 hook, device notification.
// Also contains InitialGameState + SyncHash constructors (reading from game memory).

#include "dll_hacks.hpp"
#include "asm_hacks.hpp"
#include "frame_rate.hpp"
#include "constants.hpp"
#include "messages.hpp"
#include "hash.hpp"
#include "exceptions.hpp"
#include "../common/logger.hpp"

#include "D3DHook.h"

#define INITGUID
#include <windows.h>
#include <windowsx.h>
#include <dbt.h>
#include <MinHook.h>

using namespace caster::dll::asm_hacks;

namespace caster::dll::dll_hacks {

DEFINE_GUID(GUID_DEVINTERFACE_HID, 0x4D1E55B2L, 0xF16F, 0x11CF, 0x88, 0xCB, 0x00, 0x11, 0x11, 0x00, 0x00, 0x30);

void* windowHandle = nullptr;
static HDEVNOTIFY notifyHandle = nullptr;

// Forward decl — defined in dll_main.cpp (or wherever the netplay engine lives)
extern void stopDllMain(const std::string& error);

// ---- WindowProc hook ----
using pWindowProc = LRESULT(CALLBACK*)(HWND, UINT, WPARAM, LPARAM);
static pWindowProc oWindowProc = (pWindowProc)CC_WINDOW_PROC_ADDR;

LRESULT CALLBACK WindowProcHook(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_SYSCOMMAND:
            if (wParam == SC_SCREENSAVE || wParam == SC_MONITORPOWER)
                return 0;
            break;
        case WM_KEYDOWN:
            if ((lParam >> 30) & 1) break; // ignore repeats
        case WM_SYSKEYDOWN:
            if ((HIWORD(lParam) & KF_ALTDOWN) && (wParam == VK_F4))
                stopDllMain("");
            break;
        case WM_DEVICECHANGE:
            if (wParam == DBT_DEVICEARRIVAL || wParam == DBT_DEVICEREMOVECOMPLETE) {
                if (((DEV_BROADCAST_HDR*)lParam)->dbch_devicetype == DBT_DEVTYP_DEVICEINTERFACE) {
                    // ControllerManager::refreshJoysticks() — TODO when controller manager is wired
                }
            }
            return 0;
        default:
            break;
    }
    return oWindowProc(hwnd, msg, wParam, lParam);
}

// ---- Lifecycle ----

void initializePreLoad() {
    for (const Asm& hack : hookMainLoop)       WRITE_ASM_HACK(hack);
    for (const Asm& hack : hijackControls)     WRITE_ASM_HACK(hack);
    for (const Asm& hack : hijackMenu)         WRITE_ASM_HACK(hack);
    for (const Asm& hack : detectRoundStart)   WRITE_ASM_HACK(hack);
    WRITE_ASM_HACK(multiWindow);
    WRITE_ASM_HACK(hijackEscapeKey);
    WRITE_ASM_HACK(disableTrainingMusicReset);
    WRITE_ASM_HACK(fixBossStageSuperFlashOverlay);
    caster::common::logger::info("dll_hacks: pre-load hacks applied");
}

void initializePostLoad() {
    // Enable disabled stages (needs to be after game loads)
    for (const Asm& hack : enableDisabledStages) WRITE_ASM_HACK(hack);

    // Find the game window
    windowHandle = FindWindowA(nullptr, CC_TITLE);
    if (!windowHandle)
        caster::common::logger::warn("dll_hacks: couldn't find window '{}'", CC_TITLE);

    // Register for HID device notifications (joystick hotplug)
    DEV_BROADCAST_DEVICEINTERFACE dbh{};
    dbh.dbcc_size = sizeof(dbh);
    dbh.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
    dbh.dbcc_classguid = GUID_DEVINTERFACE_HID;
    notifyHandle = RegisterDeviceNotificationA(windowHandle, &dbh, DEVICE_NOTIFY_WINDOW_HANDLE);

    // Hook WindowProc via MinHook
    MH_STATUS status = MH_Initialize();
    if (status != MH_OK) {
        caster::common::logger::warn("dll_hacks: MH_Initialize failed: {}", (int)status);
    } else {
        status = MH_CreateHook((void*)CC_WINDOW_PROC_ADDR, (void*)WindowProcHook, (void**)&oWindowProc);
        if (status != MH_OK) {
            caster::common::logger::warn("dll_hacks: MH_CreateHook failed: {}", (int)status);
        } else {
            MH_EnableHook((void*)CC_WINDOW_PROC_ADDR);
        }
    }

    // Enable frame rate control + hook DirectX Present for frame pacing.
    //
    // The DLL's frame limiter works by (1) disabling the game's native FPS
    // limiter via disableFpsLimit, then (2) installing its own limiter that
    // fires from the D3D9 Present vtable hook. Step (2) uses code-overwrite
    // vtable hooking, which does NOT work on Wine (Wine implements D3D9 over
    // OpenGL). If we did step (1) without step (2), the game would have no
    // limiter at all and run uncapped.
    //
    // So on Wine we skip both — leaving the game's native limiter intact.
    // This mirrors CCCaster's DllHacks.cpp:215-218 (isWine() early return).
    const bool isWine = [] {
        HMODULE ntdll = GetModuleHandleA("ntdll.dll");
        if (!ntdll) return false;
        return GetProcAddress(ntdll, "wine_get_version") != nullptr;
    }();

    if (isWine) {
        caster::common::logger::info("dll_hacks: Wine detected — skipping DX hook + FPS limiter (native limiter stays active)");
    } else {
        frame_rate::enable();

        // Hook DirectX (for frame sync via Present)
        if (windowHandle) {
            std::string err = InitDirectX(windowHandle);
            if (!err.empty()) {
                caster::common::logger::warn("dll_hacks: InitDirectX failed: {}", err);
            } else {
                err = HookDirectX();
                if (!err.empty())
                    caster::common::logger::warn("dll_hacks: HookDirectX failed: {}", err);
            }
        }
    }

    caster::common::logger::info("dll_hacks: post-load hacks applied");
}

void deinitialize() {
    UnhookDirectX();
    if (notifyHandle) UnregisterDeviceNotification(notifyHandle);
    if (oWindowProc) {
        MH_DisableHook((void*)CC_WINDOW_PROC_ADDR);
        MH_RemoveHook((void*)CC_WINDOW_PROC_ADDR);
        MH_Uninitialize();
        oWindowProc = nullptr;
    }
    // Revert hookMainLoop in reverse order
    for (int i = (int)hookMainLoop.size() - 1; i >= 0; --i)
        hookMainLoop[i].revert();
    caster::common::logger::info("dll_hacks: deinitialized");
}

} // namespace caster::dll::dll_hacks

// ---- InitialGameState + SyncHash constructors (read from game memory) ----
// These are defined here because they read directly from the game's address space.

namespace caster::dll {

// InitialGameState constructor that reads from game memory
void InitialGameState::readFromGame(IndexedFrame f, uint8_t state, bool training) {
    indexedFrame = f;
    stage = *(uint32_t*)CC_STAGE_SELECTOR_ADDR;
    netplayState = state;
    isTraining = training ? 1 : 0;
    chara[0] = (uint8_t)*(uint32_t*)CC_P1_CHARACTER_ADDR;
    chara[1] = (uint8_t)*(uint32_t*)CC_P2_CHARACTER_ADDR;
    moon[0]  = (uint8_t)*(uint32_t*)CC_P1_MOON_SELECTOR_ADDR;
    moon[1]  = (uint8_t)*(uint32_t*)CC_P2_MOON_SELECTOR_ADDR;
    color[0] = (uint8_t)*(uint32_t*)CC_P1_COLOR_SELECTOR_ADDR;
    color[1] = (uint8_t)*(uint32_t*)CC_P2_COLOR_SELECTOR_ADDR;
}

// SyncHash constructor that reads from game memory
void SyncHash::readFromGame(IndexedFrame f) {
    indexedFrame = f;

    // xxHash128 of RNG state
    char data[sizeof(uint32_t) * 3 + CC_RNG_STATE3_SIZE];
    std::memcpy(&data[0], (void*)CC_RNG_STATE0_ADDR, sizeof(uint32_t));
    std::memcpy(&data[4], (void*)CC_RNG_STATE1_ADDR, sizeof(uint32_t));
    std::memcpy(&data[8], (void*)CC_RNG_STATE2_ADDR, sizeof(uint32_t));
    std::memcpy(&data[12], (void*)CC_RNG_STATE3_ADDR, CC_RNG_STATE3_SIZE);
    getHash(data, sizeof(data), (char*)hash.data());

    if (*(uint32_t*)CC_GAME_MODE_ADDR != CC_GAME_MODE_IN_GAME) {
        std::memset(&chara[0], 0, sizeof(CharaHash));
        std::memset(&chara[1], 0, sizeof(CharaHash));
        chara[0].chara = (uint16_t)*(uint32_t*)CC_P1_CHARACTER_ADDR;
        chara[0].moon  = (uint16_t)*(uint32_t*)CC_P1_MOON_SELECTOR_ADDR;
        chara[1].chara = (uint16_t)*(uint32_t*)CC_P2_CHARACTER_ADDR;
        chara[1].moon  = (uint16_t)*(uint32_t*)CC_P2_MOON_SELECTOR_ADDR;
        return;
    }

    roundTimer = *(uint32_t*)CC_ROUND_TIMER_ADDR;
    realTimer  = *(uint32_t*)CC_REAL_TIMER_ADDR;
    cameraX    = *(int32_t*)CC_CAMERA_X_ADDR;
    cameraY    = *(int32_t*)CC_CAMERA_Y_ADDR;

#define SAVE_CHARA(N) \
    chara[N-1].seq          = *(uint32_t*)CC_P##N##_SEQUENCE_ADDR; \
    chara[N-1].seqState     = *(uint32_t*)CC_P##N##_SEQ_STATE_ADDR; \
    chara[N-1].health       = *(uint32_t*)CC_P##N##_HEALTH_ADDR; \
    chara[N-1].redHealth    = *(uint32_t*)CC_P##N##_RED_HEALTH_ADDR; \
    chara[N-1].meter        = *(uint32_t*)CC_P##N##_METER_ADDR; \
    chara[N-1].heat         = *(uint32_t*)CC_P##N##_HEAT_ADDR; \
    chara[N-1].guardBar     = *(uint8_t*)CC_INTRO_STATE_ADDR ? 0.0f : *(float*)CC_P##N##_GUARD_BAR_ADDR; \
    chara[N-1].guardQuality = *(float*)CC_P##N##_GUARD_QUALITY_ADDR; \
    chara[N-1].x            = *(int32_t*)CC_P##N##_X_POSITION_ADDR; \
    chara[N-1].y            = *(int32_t*)CC_P##N##_Y_POSITION_ADDR; \
    chara[N-1].chara        = (uint16_t)*(uint32_t*)CC_P##N##_CHARACTER_ADDR; \
    chara[N-1].moon         = (uint16_t)*(uint32_t*)CC_P##N##_MOON_SELECTOR_ADDR;

    SAVE_CHARA(1)
    SAVE_CHARA(2)
#undef SAVE_CHARA
}

} // namespace caster::dll
