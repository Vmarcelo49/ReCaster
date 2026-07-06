# CCCaster `Dll*` Targets — Inventory

Inventory of all `Dll*` files in `CCCaster/targets/`. Each entry lists the
filename, functionality, classification, and a port recommendation.

## Classification legend

| Tag | Meaning |
|---|---|
| **(A)** | Game-specific — depends on hardcoded MBAA.exe offsets; port requires re-mapping all addresses |
| **(B)** | Generic — logic independent of the game; port direct |
| **(C)** | Overlay/render — DX9 hook + ImGui drawing |
| **(D)** | Out of scope — not part of netplay rollback (trial, palette, etc.) |

---

## File list

| File | Functionality | LOC | Class | Port? |
|---|---|---|---|---|
| `DllMain.cpp` | Entry point + central state machine. Orchestrates NetplayManager, RollbackManager, TrialManager, sockets, frame-step (F5/F6/F7), delay/rollback hotkeys (Ctrl+0..9). `extern "C" callback()` is the per-frame hook. | ~2253 | **(A)** | Yes (heavy) |
| `DllAsmHacks.hpp` | Catalog of all ASM patches (binary patches via VirtualProtect+memcpy). `AsmList` tables for hookMainLoop, hijackControls, hijackMenu, detectRoundStart, enableDisabledStages, disableFpsLimit, hookPresentCaller, filterRepeatedSfx, etc. | ~604 | **(A)** | Yes (heavy) |
| `DllAsmHacks.cpp` | Implementation of `Asm::write/revert`, `extern "C"` callbacks (charaSelectColorCb, loadingStateColorCb, saveReplayCb, addExtraDrawCallsCb, addExtraTexturesCb, _naked_paletteCallback, _naked_presentFuncCaller), PNG palette parser, `loadCustomPalettes()`, `palettePatcher()`. | ~614 | **(A)** + **(D)** | Partial (skip palette) |
| `DllControllerManager.hpp` | Declares `DllControllerManager` class: manages keyboard/joystick mapping for 2 players, hotkey capture (F3/F4/F5), input injection into `localInputs[2]`, overlay modes for controller mapping + trial menu. | ~97 | **(B)** + **(A)** + **(D)** | Partial (skip trial) |
| `DllControllerManager.cpp` | Implementation: `initControllers()`, `updateControls()`, `handleInputEditor()`, `handleTrialMenuOverlay()`, `handleMappingOverlay()`, keyboard/joystick event handlers. | ~1055 | **(B)** + **(A)** + **(D)** | Partial (skip trial) |
| `DllControllerUtils.hpp` | Static utilities: `filterSimulDirState()` (SOCD resolution), `convertInputState()` (raw bitmask → numpad+buttons), `getPrevInput()`, `getInput()`, `isButtonPressed()`, `numJoystickButtonsDown()`. | ~108 | **(B)** | Yes (direct) |
| `DllFrameRate.hpp` | Declares namespace `DllFrameRate`: `desiredFps`, `actualFps`, `isEnabled`, `enable()`, `limitFPS()`, `PresentFrameEnd()`. | ~17 | **(A)** + **(C)** | Partial |
| `DllFrameRate.cpp` | Custom FPS limiter via QueryPerformanceCounter. Replaces game's native limiter (zeros `CC_PERF_FREQ_ADDR`). `PresentFrameEnd()` hooks DX9 Present. | ~148 | **(A)** + **(C)** | Partial |
| `DllHacks.hpp` | Declares namespace `DllHacks`: `windowHandle`, `initializePreLoad()`, `initializePostLoad()`, `deinitialize()`. | ~20 | **(A)** + **(C)** | Yes (heavy) |
| `DllHacks.cpp` | Hack lifecycle: applies AsmLists, hooks WindowProc via MinHook (keyboard/mouse/device-change), hooks DX9 via D3DHook. Constructs `InitialGameState` + `SyncHash` (reads ~25 gameplay addresses, computes MD5). | ~312 | **(A)** + **(C)** | Yes (heavy) |
| `DllNetplayManager.hpp` | Declares `NetplayManager` class: inputs containers, RNG states, retry menu indices, indexed frame, NetplayState transitions, `friend class DllRollbackManager`. | ~240 | **(A)** | Yes (heavy) |
| `DllNetplayManager.cpp` | Netplay FSM logic: `frameStepNormal()`, `getCharaSelectInput()`, `getInGameInput()`, `getRetryMenuInput()`, etc. Generates synthetic inputs for each NetplayState. UDP debug logger. | ~1268 | **(A)** | Yes (heavy) |
| `DllOverlayUi.hpp` | Declares namespace `DllOverlayUi`: enable/disable/toggle, text update (3-column), selector, message timeout, trial/mapping mode flags. | ~71 | **(C)** | Yes |
| `DllOverlayUi.cpp` | DX9 Present hook facade: `PresentFrameBegin()` (lazy-init DirectX + render text), `InitializeDirectX()`, `InvalidateDeviceObjects()`. | ~86 | **(C)** | Yes |
| `DllOverlayUiImGui.cpp` | ImGui demo window rendering (debug builds only). `initImGui()`, `EndScene()` with NewFrame/Render/RenderDrawData. | ~116 | **(C)** | Yes (trivial) |
| `DllOverlayUiText.cpp` | 3-column text overlay + trial combo text via D3D9 (`ID3DXFont` + vertex buffer for translucent background). Height animation, message timeouts, trial combo list rendering. | ~535 | **(C)** + **(D)** | Partial (skip trial) |
| `DllOverlayPrimitives.hpp` | Static D3D9 draw helpers: `DrawRectangle()`, `DrawBox()`, `DrawCircle()`, `DrawText()` / `DrawTextW()` wrappers, `TextCalcRect()`. | ~103 | **(C)** | Yes (direct) |
| `DllPaletteManager.cpp` | Bridge between AsmHacks palette callbacks and `PaletteManager` class. `colorLoadCallback()` loads/applies custom palettes from PNG files. | ~50 | **(D)** | No |
| `DllProcessManager.cpp` | DLL-side `ProcessManager` methods: `writeGameInput()` (writes to `CC_PTR_TO_WRITE_INPUT_ADDR` + offsets), `getRngState()` / `setRngState()` (reads/writes 4 RNG fields), `connectPipe()` (named pipe IPC with launcher). | ~117 | **(A)** | Yes (critical) |
| `DllRollbackManager.hpp` | Declares `DllRollbackManager` class + `GameState` struct: save/load game state via memory pool, SFX history for mute-during-reroll, replay input container fixup. | ~87 | **(A)** | Yes (heavy) |
| `DllRollbackManager.cpp` | Implementation: `allocateStates()` (loads `binary_res_rollback_bin`), `saveState()`, `loadState()` (restores + fixes `RepInputContainer`), `saveRerunSounds()`, `finishedRerunSounds()`. | ~262 | **(A)** | Yes (heavy) |
| `DllSpectatorManager.cpp` | `SpectatorManager` class: list/map of spectator sockets, round-robin broadcast of BothInputs/RngState/RetryMenuIndex at throttled interval, `getRandomSpectatorAddress()` for redirect. | ~235 | **(B)** + **(A)** (light) | Yes |
| `DllTrialManager.hpp` | Trial mode (combo training): enums, structs (Token, Move, DemoInput, Trial), namespace globals (combo text, demo inputs, input editor state). | ~211 | **(D)** | No |
| `DllTrialManager.cpp` | Trial mode implementation: `frameStepTrial()`, `loadTrialFile()`, `loadCombo()`, ~30 `draw*` methods using game's internal draw functions. | ~1885 | **(D)** | No |
| `oCallDraw.c` | C wrapper calling MBAA's internal draw functions (drawtext @ `0x41d340`, drawsprite @ `0x415580`, drawrect @ `0x415450`, createTexFromFileInMemory @ `0x4bd2d0`). Source for `CallDraw.s`. | ~36 | **(A)** + **(D)** | No |
| `CallDraw.s` | Assembly (MinGW i686) generated from `oCallDraw.c`. Hardcoded function pointers as `.long` literals. | ~179 | **(A)** + **(D)** | No |

---

## Summary by classification

| Classification | Files | Total LOC | Port recommendation |
|---|---|---|---|
| **(A) Game-specific** | `DllMain.cpp`, `DllAsmHacks.{hpp,cpp}`, `DllHacks.{hpp,cpp}`, `DllNetplayManager.{hpp,cpp}`, `DllProcessManager.cpp`, `DllRollbackManager.{hpp,cpp}` | ~5317 | Port manually — requires `Constants.hpp` with all `CC_*_ADDR` offsets for the target game version |
| **(B) Generic** | `DllControllerManager.{hpp,cpp}` (partial), `DllControllerUtils.hpp`, `DllSpectatorManager.cpp` (partial) | ~1495 | Port direct — abstract away `CC_WORLD_TIMER_ADDR` and `CC_P*_FACING_FLAG_ADDR` |
| **(C) Overlay/render** | `DllOverlayUi.{hpp,cpp}`, `DllOverlayUiImGui.cpp`, `DllOverlayUiText.cpp` (partial), `DllOverlayPrimitives.hpp`, `DllFrameRate.{hpp,cpp}` (partial) | ~970 | Port after rewriting D3D9 hook mechanism; `CC_SCREEN_WIDTH_ADDR` replaceable by viewport param |
| **(D) Out of scope** | `DllTrialManager.{hpp,cpp}`, `DllPaletteManager.cpp`, `oCallDraw.c`, `CallDraw.s` | ~2122 | Do not port — trial/palette features are not part of netplay rollback |

**Total analyzed**: ~9904 lines across 26 files.

---

## Recommended port order (for netplay rollback)

If we want to bring ReCaster to feature parity with CCCaster's netplay,
the recommended order is:

1. **`Constants.hpp`** (not a `Dll*` file, but prerequisite) — all
   `CC_*_ADDR` offsets for MBAA.exe 1.07 Rev.1.4.0. Without this, nothing
   else can be ported. ~80 addresses.

2. **`DllProcessManager.cpp`** (~117 lines) — input injection + RNG
   read/write + IPC pipe. This is the smallest critical piece. Enables
   the DLL to actually control the game.

3. **`DllControllerUtils.hpp`** + **`DllControllerManager.{hpp,cpp}`**
   (~1260 lines, partial) — controller input reading + SOCD filtering +
   input conversion. Reuses our existing `mapping.ini` format.

4. **`DllNetplayManager.{hpp,cpp}`** (~1508 lines) — the netplay FSM.
   This is the brain: menu navigation, auto-chara-select, input
   generation per state, round detection, etc.

5. **`DllRollbackManager.{hpp,cpp}`** (~349 lines) + the
   `binary_res_rollback_bin` resource — state save/restore. This is the
   hardest part because the rollback_bin blob is a compiled list of
   (address, size) pairs that must match the exact game version.

6. **`DllAsmHacks.{hpp,cpp}`** (~1218 lines, partial) — all the ASM
   patches. Skip palette-related patches (they're category D).

7. **`DllHacks.{hpp,cpp}`** (~332 lines) — hack lifecycle + WindowProc
   hook + DX9 hook + SyncHash/InitialGameState.

8. **`DllMain.cpp`** (~2253 lines) — the central orchestrator that ties
   everything together. Port last, after all the pieces it depends on
   are in place.

9. **`DllSpectatorManager.cpp`** (~235 lines) — spectator support. Can
   be done in parallel with the above, since it's mostly generic socket
   broadcast logic.

10. **DX9 overlay** (`DllOverlayUi*.{hpp,cpp}` + `DllOverlayPrimitives.hpp`,
    ~825 lines) — optional but useful for debug info and netplay status
    display inside the game.

**Estimated total effort**: ~7000 lines of C++ to port, plus the
`Constants.hpp` and `rollback_bin` reverse-engineering. This is a
significant undertaking — roughly equivalent to all of phases 1-9
combined.
