// src/dll/entry/lifecycle.cpp
// Ported from CCCaster DllHacks.cpp. Reimplemented: minimal overlay init.
// Kept: ASM patch lifecycle, WindowProc hook via MinHook, DX9 hook, device notification.
// Also contains InitialGameState + SyncHash constructors (reading from game memory).

#include "lifecycle.hpp"
#include "hooks/asm_patches.hpp"
#include "hooks/frame_limiter.hpp"
#include "overlay/overlay_ui.hpp"
#include "overlay/keymapper.hpp"
#include "overlay/playername_overlay.hpp"
#include "game/addresses.hpp"
#include "protocol/messages.hpp"
#include "util/hash.hpp"
#include "util/exceptions.hpp"
#include "../common/ipc/config_buffer.hpp"
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
        case WM_KEYDOWN: {
            if ((lParam >> 30) & 1) break; // ignore repeats

            // Alt+F4 → stopDllMain (graceful exit)
            if ((HIWORD(lParam) & KF_ALTDOWN) && (wParam == VK_F4)) {
                stopDllMain("");
                break;
            }

            // Forward to keymapper first — if it's capturing a key, it
            // consumes the event and we don't process it further.
            if (caster::dll::overlay::keymapper::handleKeyEvent(
                    static_cast<uint32_t>(wParam), /*isDown=*/true)) {
                break;
            }

            // Top-row number keys (NOT numpad) drive overlay modes.
            // VK_1..VK_9 are 0x31..0x39. We reserve:
            //   1, 2 — future features (stubs for now)
            //   3    — toggle the info overlay on/off
            //   4    — toggle the controller-mapping overlay
            //   5    — toggle the playername overlay
            //
            // NOTE: The actual toggle logic lives in the GetAsyncKeyState
            // polling in dll_main.cpp's callback(). We do NOT call toggle()
            // here because that would cause a double-toggle (poll + WM_KEYDOWN
            // both firing for the same keypress), leaving g_mode back at
            // Inactive while the overlay text stays stale.
            //
            // The WM_KEYDOWN path is kept only for the Alt+F4 exit handler
            // above. Hotkeys 1-5 are poll-driven exclusively.
            switch (wParam) {
                case '1': // 0x31
                case '2': // 0x32
                case '3': // 0x33
                case '4': // 0x34
                case '5': // 0x35
                    // Handled by polling in callback() — do nothing here.
                    break;
                default:
                    break;
            }
            break;
        }
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

    // Frame rate control + DirectX Present hook.
    //
    // The DLL's frame limiter works by (1) disabling the game's native FPS
    // limiter via disableFpsLimit, then (2) installing its own limiter that
    // fires from the D3D9 Present vtable hook. On Wine, step (2) works (we
    // use vtable swap, which is Wine-compatible), but step (1) isn't needed
    // because Wine's D3D9 already paces frames via the host compositor —
    // disabling the native limiter there would leave the game uncapped.
    // So on Wine we skip step (1) and leave the native limiter intact.
    //
    // The DX9 Present hook itself is installed on ALL platforms (native
    // Windows AND Wine) because the in-game overlay depends on it —
    // overlay::presentFrameBegin() is called from our DX9_Present hook.
    const bool isWine = [] {
        HMODULE ntdll = GetModuleHandleA("ntdll.dll");
        if (!ntdll) return false;
        return GetProcAddress(ntdll, "wine_get_version") != nullptr;
    }();

    if (!isWine) {
        // Native Windows: enable custom frame limiter (disables game's native
        // limiter + installs Present hook for frame pacing).
        frame_rate::enable();
    } else {
        caster::common::logger::info("dll_hacks: Wine detected — skipping frame_rate::enable() (native limiter stays active)");
    }

    // Hook DirectX Present (vtable swap). Used by both the frame limiter on
    // native Windows AND by the overlay on all platforms.
    //
    // InitDirectX only uses `hwnd` as the focus window for a temporary D3D9
    // device used to read vtable addresses. If the game window isn't found
    // yet (Wine timing, or Community Edition with a different title), we
    // pass the desktop window — the vtable addresses are the same regardless.
    void* dxHwnd = windowHandle;
    if (!dxHwnd) {
        dxHwnd = GetDesktopWindow();
        caster::common::logger::info("dll_hacks: windowHandle is null, using desktop window for DX init");
    }
    std::string err = InitDirectX(dxHwnd);
    if (!err.empty()) {
        caster::common::logger::warn("dll_hacks: InitDirectX failed: {}", err);
    } else {
        err = HookDirectX();
        if (!err.empty())
            caster::common::logger::warn("dll_hacks: HookDirectX failed: {}", err);
        else
            caster::common::logger::info("dll_hacks: DX9 hook installed{}", isWine ? " (Wine)" : "");
    }

    // Arm the overlay for lazy init on the first Present call. Start
    // DISABLED — the user toggles it on with the '3' hotkey. The actual
    // font + VB are created on the first Present call, when a valid
    // IDirect3DDevice9* is available. If the DX hook failed above,
    // presentFrameBegin() is never called and the overlay stays invisible
    // (no crash, no resource allocation).
    overlay::init();
    overlay::updateText({ "ReCaster", "DX9 Overlay v0.1", "" });
    // overlay::enable() — not called; overlay starts disabled.
    caster::common::logger::info("dll_hacks: overlay armed (starts disabled, press '3' to toggle)");

    // Note: playername::init() is called from dll_main.cpp's doIpcAndModePatch()
    // after the IPC config is received, because it needs the overlay flags from
    // the config buffer.

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
