# Non-Implemented Stubs

This document lists all features that are currently stubbed or not yet
implemented in ReCaster. Each entry describes what's missing, why, and
what it would take to implement.

> **FOCO ATUAL**: As entradas #6 (Rollback) e #7 (FSM de netplay) são o
> foco ativo de desenvolvimento e têm um plano de execução dedicado em
> [`docs/phase-f-execution-plan.md`](phase-f-execution-plan.md). Esse
> plano detalha as divergências linha-a-linha contra o CCCaster e a
> sequência de 7 sub-etapas para completar a Fase F.

---

## 1. Game-specific ASM patches (`apply_game_patches`) — IMPLEMENTED

**Location**: `src/exe/launcher/launcher.cpp` → `apply_game_patches()`

**Status**: ✅ Implemented.

`apply_game_patches()` (`launcher.cpp:128-166`) applies three patches to
the suspended MBAA.exe process, before `ResumeThread`:

| Address | Bytes | Effect |
|---|---|---|
| `0x04A1D42` | `0xEB, 0x0E` | Skip config dialog (part 1) |
| `0x04A1D4A` | `0xEB` | Skip config dialog (part 2) |
| `0x42B475` | `0xEB, 0x22` (training) / `0xEB, 0x3F` (versus) | Force direct jump to Training or Versus mode |

A `training` flag selects between the two forceGoto variants. No version
check is performed — the patches assume MBAA.exe 1.07 Rev.1.4.0 (the
only supported version).

---

## 2. Spectate mode (direct + relay) — PARTIAL

**Location**: `src/exe/pages/play_page.cpp` → `do_spectate()`, `src/exe/cli.cpp` → `run_netplay(Spectate)`

**What's stubbed**:
- GUI `do_spectate()` (`play_page.cpp:153-178`) returns `"Not yet implemented"` for direct spectate and `"Spectate via relay not supported yet"` for relay spectate.
- CLI `run_netplay(Spectate)` (`cli.cpp:149-176`) rejects relay spectate outright (`"spectate via relay not supported yet"`). Direct spectate falls through to `session.start_join(...)` — i.e. it joins as a regular player, not as a spectator. The `is_spectator` flag in `NetplayConfig` (`netplay_config.hpp:17`) is never set to `true` by any caller.

**What it should do**: Connect to a host as a spectator — receive inputs and game state but don't send any. The host runs a `SpectatorManager` that broadcasts `BothInputs` / `RngState` / `RetryMenuIndex` messages to connected spectators at a throttled interval.

**Why it's stubbed**: Spectating requires:
1. A `SpectatorManager` on the host side (the host must accept spectator connections and broadcast state to them). No such class exists in ReCaster yet.
2. A spectator-mode flag in the `NetplaySession` that changes the handshake protocol (spectators don't exchange pings or configs — they just receive).
3. The DLL must run in spectator mode (no input injection, just replay).

**What it would take**: Port `DllSpectatorManager.cpp` from CCCaster (~235 lines, mostly generic socket broadcast logic). Add a spectator flag to `NetplayConfig` (already exists as `is_spectator`, but unused) and wire it through the IPC `config_buffer`. Modify the handshake to skip ping/config exchange for spectators. This is a moderate effort — the relay infrastructure is already in place.

---

## 3. Manual delay override (`--delay=N`) — STUB

**Location**: `src/exe/cli.cpp` → `run_netplay()`

**What's stubbed**: The `--delay=N` CLI flag is parsed and validated (`cli_args.cpp:62-66` enforces 0..8), but the value is only logged — it's not actually applied to the `NetplaySession`. `cli.cpp:189-196` explicitly states: `"manual delay override = N frames (not yet applied to session)"`. The session always uses auto-computed delay (from RTT) on the host side.

**What it should do**: When `--delay=N` is provided, the host should use `N` as the input delay instead of computing it from RTT. The `NetplayConfig` struct already has a `manual_delay` bool field (`netplay_config.hpp:19`), and the auto-compute in `session.cpp:712` is already gated on `!config_.manual_delay` — so the session side is ready, it just never receives the flag.

**Why it's stubbed**: The session's internal `NetplayConfig` is private. The missing piece is just a setter and a call site.

**What it would take**: Add `NetplaySession::set_manual_delay(uint8_t delay)` that sets `config_.manual_delay = true` and `config_.delay = delay`. Call it from `cli.cpp` before `session.start_*()` (around line 188, where the `manual_delay` local is currently computed but discarded). ~5 lines of code in `session.hpp` + 2 lines in `cli.cpp`.

---

## 4. Display name fallback (from game's NetConnect.dat) — STUB

**Location**: `src/common/config.hpp` → `Config::display_name`, `src/common/config.cpp`

**What's stubbed**: When `display_name` is empty in `config.ini`, the launcher should fall back to reading the player's name from the game's `System/NetConnect.dat` file. This fallback is not implemented. `config.hpp:45` documents this with the comment `"empty → fallback (not yet implemented)"`.

**What it should do**: If `display_name` is empty after loading config, parse `System/NetConnect.dat` (a binary file in the game's directory) and extract the stored player name. The constant `CC_NETWORK_CONFIG_FILE = "System\\NetConnect.dat"` is already defined in `src/dll/constants.hpp:39` along with `CC_NETWORK_USERNAME_KEY = "UserName"` (`constants.hpp:40`), suggesting the key name is known.

**Why it's stubbed**: The `NetConnect.dat` format is game-specific and not yet documented. The zzcaster implementation reads it via `config.fetchGameUserName()`.

**What it would take**: Reverse-engineer or port the `NetConnect.dat` parser from zzcaster's `common/config.zig`. ~30 lines of code once the format is known. Likely a small INI-like or fixed-offset binary format given the existing key-name constant.

---

## 5. DX9 overlay (ImGui inside the game) — STUB

**Location**: `src/dll/dll_main.cpp` (callbacks `PresentFrameBegin`, `EndScene`, `InvalidateDeviceObjects`)

**What's stubbed**: The DX9 overlay is not implemented. `dll_main.cpp:295-297` defines the D3DHook callbacks as empty functions:

```cpp
void PresentFrameBegin(IDirect3DDevice9*) {}
void EndScene(IDirect3DDevice9*) {}
void InvalidateDeviceObjects() {}
```

Only `PresentFrameEnd` (`dll_main.cpp:298-300`) does anything — it calls `frame_rate::PresentFrameEnd(device)`, which runs the custom FPS limiter. The hook on `IDirect3DDevice9::Present` is installed and active (for frame sync), but no rendering happens inside it.

**What it should do**: Hook `IDirect3DDevice9::Present` (or `EndScene`) via vtable hooking, create an ImGui context bound to the game's D3D9 device, and render overlay UI (debug info, netplay status, rollback metrics) directly on top of the game's framebuffer.

**Why it's stubbed**: This is a large feature (~600+ lines) that requires:
1. D3D9 vtable hooking (intercept `Present` / `EndScene`) — infrastructure exists via `3rdparty/d3dhook/`, but only `Present` is currently hooked.
2. `imgui_impl_dx9.cpp` + `imgui_impl_win32.cpp` compiled into `hook.dll`.
3. Window message hooking (WindowProc via MinHook or similar) to feed keyboard/mouse events to ImGui. (A WindowProc hook already exists in `dll_hacks.cpp`, but it doesn't route events to ImGui.)
4. Device lost/reset handling (DX9 device can be lost on alt-tab).

**What it would take**: Port `DllOverlayUi*.cpp` + the overlay portion of `DllHacks.cpp` from CCCaster. The CCCaster code uses MinHook for WindowProc hooking and a direct vtable patch for `Present`. See `docs/cccaster-dll-targets.md` for the full inventory of relevant files.

---

## 6. Rollback netplay (state save/restore) — PARTIAL

**Location**: `src/dll/rollback_manager.{hpp,cpp}`, `src/dll/rollback_addresses.hpp`, `src/dll/dll_main.cpp` (frameStep)

**What's implemented**:
- `RollbackManager` class is complete: `allocateStates()`, `deallocateStates()`, `saveState(state, worldTime, indexedFrame)`, `loadState(target, outState, outWorldTime, outIndexedFrame)`, `hasStates()` (`rollback_manager.hpp`, `rollback_manager.cpp`).
- `rollback_addresses.hpp` builds the `MemDumpList` at runtime (replaces CCCaster's `binary_res_rollback_bin` blob). Contains ~80 hardcoded addresses for MBAA.exe 1.07.
- `MemDump` (`mem_dump.hpp`) handles the actual save/restore of memory ranges.
- `RngState` save/restore via `process_manager::getRngState()` / `setRngState()` is also in place.

**What's stubbed / missing**: `RollbackManager` is **not plugged into the frame loop**. A search across all `.cpp` files finds zero call sites for `saveState` / `loadState` outside the class itself. The `frameStep()` function in `dll_main.cpp:189-272` runs a simple "read local input → write to game → send to peer → apply remote input" loop with no state snapshotting, no rollback trigger, and no replay of intervening frames. The `delay` and `rollback` fields from the IPC config are received and logged but ignored (currently delay=0 is effectively used).

**What it should do**: On each frame in `InGame` state, save the game state via `saveState()`. When a remote input arrives that disagrees with a predicted input (the `InputsContainer::getLastChangedFrame` returns a frame earlier than expected), roll back via `loadState()`, replay the intervening frames with the corrected inputs, then resume. Apply `cfg.delay` as the input delay offset when reading the remote input from the `InputsContainer`. This is the core of GGPO-style rollback netplay.

**Why it's stubbed**: Plugging rollback into the frame loop requires the full netplay FSM (stub #7) to be in place — rollback decisions depend on knowing the current `NetplayState` (only `InGame` and `Skippable` should snapshot), the indexed-frame counter (which requires the FSM to track frame indexing), and the round-start detection (which determines when to call `allocateStates()`).

**What it would take**: Once stub #7 lands the FSM with proper indexed-frame tracking and `NetplayState` transitions, plugging rollback is moderate effort: call `allocateStates()` on `Loading → CharaIntro/InGame` transition, `saveState()` each in-game frame, `loadState()` when the `InputsContainer` detects a remote-input divergence. Estimated ~150-200 lines of integration code in `dll_main.cpp::frameStep`. **Plano detalhado em [phase-f-execution-plan.md](phase-f-execution-plan.md) — Etapa F.5**, incluindo correções necessárias em `rollback_addresses.hpp` (pointer chasers ausentes) e em `RollbackManager::saveState`/`loadState` (estratégia de eviction e fallback divergem do CCCaster).

---

## 7. Netplay state machine (DLL-side) — PARTIAL

**Location**: `src/dll/netplay_states.hpp`, `src/dll/dll_main.cpp` (frameStep), `src/dll/netplay_connector.{hpp,cpp}`

**What's implemented**:
- `NetplayState` enum (`netplay_states.hpp:21-32`) and `isValidNextState()` transition validator (`netplay_states.hpp:51-80`) are complete.
- `netplayStateStr()` debug formatter (`netplay_states.hpp:34-48`).
- The DLL tracks `gameMode` (a separate MBAA-specific value, not `NetplayState`) and runs a small menu-navigation state machine in `dll_main.cpp:204-237` that mashes Confirm through Startup/Opening/Title/Main/LoadingDemo/HighScores until `forceGoto` takes effect. This is the port of CCCaster's `getPreInitialInput()` / `getInitialInput()`.
- Past the menu, `frameStep` (`dll_main.cpp:239-271`) reads the local controller and (in netplay mode) exchanges inputs with the peer via `netplay_connector` — `send_local_input()` + `apply_remote_input()`.
- `SyncHash::readFromGame()` (`dll_hacks.cpp:182`) and `InitialGameState::readFromGame()` (`dll_hacks.cpp:168`) are implemented and read from game memory, but neither is sent over the wire or compared.

**What's stubbed / missing**: There is no `NetplayManager` class (the "brain"). Specifically missing:
1. **FSM driver** — code that maps `gameMode` → `NetplayState` and drives transitions explicitly. Currently `frameStep` only checks "am I in menu flow?" with a boolean; it doesn't track `NetplayState` at all.
2. **Auto-chara-select** — synthetic inputs to navigate the chara-select screen (select character, moon, color, then Confirm). Not implemented — currently the local controller must do it manually.
3. **Round-start detection** — `detectRoundStart` ASM patch exists in `asm_hacks`, but the per-frame callback that reads `roundStartCounter` and triggers `allocateStates()` + state transition `CharaIntro → InGame` is not wired up.
4. **RNG sync + `match_seed`** — `setRngState()` exists but is never called. `match_seed` is received via IPC and logged but never applied to the game.
5. **`SyncHash` exchange + desync detection** — `SyncHash::readFromGame()` works, but no code sends it to the peer or compares incoming hashes.
6. **Retry menu navigation** — synthetic inputs for `RetryMenu → Loading` (rematch) or `RetryMenu → CharaSelect` (chara change). Not implemented.
7. **Spectator mode on the DLL side** — no code path that receives `BothInputs` for both players without injecting local input.

**Why it's stubbed**: This is the most game-specific code in the entire project. The full CCCaster `DllNetplayManager.cpp` is ~1268 LOC and reads/writes ~80 hardcoded memory addresses in MBAA.exe (chara selectors, moon selectors, color selectors, game mode, world timer, stage selector, pause flag, facing flags, positions, sequences, health, meter, heat, guard bars, camera, RNG state, etc.). Every address is version-specific.

**What it would take**: Port `DllNetplayManager.{hpp,cpp}` (~1508 lines) + the FSM portion of `DllMain.cpp` (~2253 lines) from CCCaster. The `Constants.hpp` with all `CC_*_ADDR` offsets is already in place (`src/dll/constants.hpp`), and `RollbackManager` / `InputsContainer` / `process_manager` are ready to be called. The hard part is the per-state input-generation logic — each `NetplayState` has its own synthetic-input recipe that must match the game's menu timing exactly. **Plano detalhado em [phase-f-execution-plan.md](phase-f-execution-plan.md) — Etapas F.2, F.3, F.6**, incluindo correções necessárias em `InputsContainer` (semântica diverge do CCCaster) e em `netplay_states.hpp` (tabela de transições diverge).

---

## 8. Controller input injection (DLL-side) — IMPLEMENTED

**Location**: `src/dll/input_reader.{hpp,cpp}`, `src/dll/dll_process_manager.{hpp,cpp}`, `src/dll/dll_main.cpp` (frameStep)

**Status**: ✅ Implemented and validated against MBAA.exe via Wine.

- `read_local_input(SDL_Joystick*, ControllerMapping&) → GameInput` (`input_reader.cpp:92`) reads SDL_Joystick + Win32 keyboard state, applies the same `ControllerMapping` format the launcher GUI uses (loaded from `caster/mapping.ini`), filters SOCD, and returns `{direction, buttons}` in the MBAA numpad+buttons format.
- `process_manager::writeGameInput(player, direction, buttons)` (`dll_process_manager.cpp:11`) writes to `CC_PTR_TO_WRITE_INPUT_ADDR` (0x76E6AC) + offsets `0x18` (P1 direction) / `0x24` (P1 buttons) / `0x2C` (P2 direction) / `0x38` (P2 buttons).
- `frameStep()` in `dll_main.cpp:252-270` calls `read_local_input` and `writeGameInput` every frame for both offline (training/versus) and netplay modes. In netplay mode it also calls `netplay::send_local_input()` and `netplay::apply_remote_input()` to exchange inputs with the peer.
- Validated evidence in `debug.log`: SDL joystick subsystem is initialized inside the DLL, controller directional + button inputs reach the game.

---

## 9. Dead code: `src/dll/dll_stubs.cpp`

**Location**: `src/dll/dll_stubs.cpp`

**What's stubbed**: This file defines `callback()`, `stopDllMain()`, `PresentFrameBegin()`, `EndScene()`, `InvalidateDeviceObjects()`, and `PresentFrameEnd()` — all of which are **also defined** in `src/dll/dll_main.cpp`.

**Status**: The file is **dead code**. `CMakeLists.txt:261` compiles `src/dll/dll_main.cpp` and does NOT compile `dll_stubs.cpp`. Including both in the build would cause duplicate-symbol link errors. The file is a leftover from before `dll_main.cpp` was implemented (its header comment says "stubs for symbols... will be replaced when the netplay engine is ported").

**What it would take**: Delete the file. It serves no purpose and is misleading — anyone reading it might think the DLL still runs empty stubs, when in fact the real implementation lives in `dll_main.cpp`.
