# Non-Implemented Stubs

This document lists all features that are currently stubbed or not yet
implemented in ReCaster. Each entry describes what's missing, why, and
what it would take to implement.

---

## 1. Game-specific ASM patches (`apply_game_patches`)

**Location**: `src/exe/launcher/launcher.cpp` → `apply_game_patches()`

**What's stubbed**: The function is a no-op — it logs "no patches applied
(stub)" and returns `true` without modifying the game's memory.

**What it should do**: Apply version-specific ASM patches to MBAA.exe
right after DLL injection (while the process is still suspended) and
before `ResumeThread`. The known zzcaster patches are:

| Address | Bytes | Effect |
|---|---|---|
| `0x04A1D42` | `0xEB, 0x0E` | JMP +14 — skip config dialog (part 1) |
| `0x04A1D4A` | `0xEB` | JMP short — skip config dialog (part 2) |

Image base for MBAACC: `0x00400000` (standard for 32-bit exes). The patch
addresses above are absolute VAs.

**Why it's stubbed**: The patches are version-specific to MBAACC 1.07
Rev.1.4.0. They need to be validated against the exact binary the user
is running, and the skip-config-dialog behavior may not be desirable in
all scenarios.

**What it would take**: Uncomment the `memory::patch_memory` calls in
`apply_game_patches()` (the code is already written as a comment). Add a
version check (read the PE version resource or a known signature at a
fixed offset) before applying, and log a warning if the version doesn't
match.

---

## 2. Spectate mode (direct + relay)

**Location**: `src/exe/pages/play_page.cpp` → `do_spectate()`, `src/exe/cli.cpp` → `run_netplay(Spectate)`

**What's stubbed**: Both the GUI and CLI spectate paths log "not yet
implemented" and return without starting a session. Relay spectate
(`--spec=#room`) is explicitly rejected as unsupported.

**What it should do**: Connect to a host as a spectator — receive inputs
and game state but don't send any. The host runs a `SpectatorManager`
that broadcasts `BothInputs` / `RngState` / `RetryMenuIndex` messages to
connected spectators at a throttled interval.

**Why it's stubbed**: Spectating requires:
1. A `SpectatorManager` on the host side (the host must accept spectator
   connections and broadcast state to them).
2. A spectator-mode flag in the `NetplaySession` that changes the
   handshake protocol (spectators don't exchange pings or configs —
   they just receive).
3. The DLL must run in spectator mode (no input injection, just replay).

**What it would take**: Port `DllSpectatorManager.cpp` from CCCaster
(~235 lines, mostly generic socket broadcast logic). Add a spectator
flag to `NetplayConfig` and `config_buffer`. Modify the handshake to
skip ping/config exchange for spectators. This is a moderate effort —
the relay infrastructure is already in place.

---

## 3. Manual delay override (`--delay=N`)

**Location**: `src/exe/cli.cpp` → `run_netplay()`

**What's stubbed**: The `--delay=N` CLI flag is parsed and validated,
but the value is only logged — it's not actually applied to the
`NetplaySession`. The session always uses auto-computed delay (from
RTT) on the host side.

**What it should do**: When `--delay=N` is provided, the host should
use `N` as the input delay instead of computing it from RTT. The
`NetplayConfig` struct already has a `manual_delay` bool field, but
there's no `session.set_manual_delay(N)` setter.

**Why it's stubbed**: The session's internal `NetplayConfig` is private.
Adding a setter is trivial, but the manual_delay flag needs to be
checked in `finish_ping_exchange()` (it already is — the auto-compute
is skipped when `manual_delay` is true). The missing piece is just
wiring the CLI value into the session before `start_*()` is called.

**What it would take**: Add `NetplaySession::set_manual_delay(uint8_t delay)`
that sets `config_.manual_delay = true` and `config_.delay = delay`.
Call it from `cli.cpp` before `session.start_*()`. ~5 lines of code.

---

## 4. Display name fallback (from game's NetConnect.dat)

**Location**: `src/common/config.hpp` → `Config::display_name`

**What's stubbed**: When `display_name` is empty in `config.ini`, the
launcher should fall back to reading the player's name from the game's
`System/NetConnect.dat` file. This fallback is not implemented.

**What it should do**: If `display_name` is empty after loading config,
parse `System/NetConnect.dat` (a binary file in the game's directory)
and extract the stored player name.

**Why it's stubbed**: The `NetConnect.dat` format is game-specific and
not yet documented. The zzcaster implementation reads it via
`config.fetchGameUserName()`.

**What it would take**: Reverse-engineer or port the `NetConnect.dat`
parser from zzcaster's `common/config.zig`. ~30 lines of code once the
format is known.

---

## 5. DX9 overlay (ImGui inside the game)

**Location**: N/A (not yet started)

**What's stubbed**: The entire DX9 overlay is not implemented. The
`hook.dll` currently opens its own SDL2+ImGui window alongside the
game, rather than rendering ImGui inside the game's DirectX 9 framebuffer.

**What it should do**: Hook `IDirect3DDevice9::Present` (or `EndScene`)
via vtable hooking, create an ImGui context bound to the game's D3D9
device, and render overlay UI (debug info, netplay status, rollback
metrics) directly on top of the game's framebuffer.

**Why it's stubbed**: This is a large feature (~600+ lines) that
requires:
1. D3D9 vtable hooking (intercept `Present` / `EndScene`).
2. `imgui_impl_dx9.cpp` + `imgui_impl_win32.cpp` compiled into `hook.dll`.
3. Window message hooking (WindowProc via MinHook or similar) to feed
   keyboard/mouse events to ImGui.
4. Device lost/reset handling (DX9 device can be lost on alt-tab).

**What it would take**: Port `DllOverlayUi*.cpp` + `DllHacks.cpp` (DX
hook portion) from CCCaster. The CCCaster code uses MinHook for
WindowProc hooking and a direct vtable patch for `Present`. See
`docs/cccaster-dll-targets.md` for the full inventory of relevant files.

---

## 6. Rollback netplay (state save/restore)

**Location**: N/A (not yet started)

**What's stubbed**: The `hook.dll` does not implement rollback — it
doesn't save/restore game state, doesn't replay inputs, and doesn't
do frame prediction. The IPC config buffer carries `delay` and
`rollback` fields, but the DLL ignores them.

**What it should do**: On each frame, save the game state (via a list
of memory addresses to snapshot). When a remote input arrives that
disagrees with a predicted input, roll back to the last confirmed
state, replay the intervening frames with the corrected inputs, then
resume. This is the core of GGPO-style rollback netplay.

**Why it's stubbed**: Rollback requires:
1. A list of memory addresses to save/restore (the `rollback_bin`
   resource in CCCaster — a compiled binary blob listing ~80 addresses
   + sizes specific to MBAA.exe 1.07).
2. A `RollbackManager` that manages a pool of `NUM_ROLLBACK_STATES`
   (60 in release) state snapshots.
3. SFX history tracking (mute sounds during rollback reroll to avoid
   audio glitches).
4. Replay input container fixup (the game's internal replay struct
   needs its frame counts adjusted after rollback).

**What it would take**: Port `DllRollbackManager.{hpp,cpp}` from
CCCaster (~349 lines). The hard part is regenerating the `rollback_bin`
resource — it's a binary blob of `(address, size)` pairs that needs to
be extracted from the game binary via analysis tools. Without it,
rollback can't save/restore the right memory regions. This is the
single biggest piece of missing functionality.

---

## 7. Netplay state machine (DLL-side)

**Location**: N/A (not yet started)

**What's stubbed**: The `hook.dll` does not implement the MBAA-specific
netplay state machine (`NetplayState` FSM: PreInitial → Initial →
AutoCharaSelect → CharaSelect → Loading → CharaIntro → Skippable →
InGame → RetryMenu → ReplayMenu). It also doesn't write inputs to the
game's memory, doesn't read game state for sync hashing, and doesn't
generate auto-chara-select inputs.

**What it should do**: Drive the game through the netplay lifecycle:
auto-navigate menus, inject local inputs, send remote inputs, detect
round starts, manage RNG sync, etc.

**Why it's stubbed**: This is the most game-specific code in the entire
project. It reads/writes ~80 hardcoded memory addresses in MBAA.exe
(chara selectors, moon selectors, color selectors, game mode, world
timer, stage selector, pause flag, facing flags, positions, sequences,
health, meter, heat, guard bars, camera, RNG state, etc.). Every
address is version-specific.

**What it would take**: Port `DllNetplayManager.{hpp,cpp}` (~1508 lines)
+ `DllMain.cpp` (~2253 lines) + `DllProcessManager.cpp` (~117 lines)
+ `DllHacks.cpp` (SyncHash/InitialGameState, ~332 lines) from CCCaster.
This requires the complete `Constants.hpp` with all `CC_*_ADDR` offsets
for the target game version. See `docs/cccaster-dll-targets.md` for
details.

---

## 8. Controller input injection (DLL-side)

**Location**: N/A (not yet started)

**What's stubbed**: The `hook.dll` receives the IPC config but doesn't
actually inject controller inputs into the game. The launcher's
controller mapper UI works (it saves `mapping.ini`), but the mapping is
never consumed by the DLL.

**What it should do**: Read the local player's controller via SDL
(using the `mapping.ini` bindings), convert to the game's numpad+buttons
format, and write to the game's input addresses (`CC_P1_OFFSET_DIRECTION`
/ `CC_P1_OFFSET_BUTTONS` via `CC_PTR_TO_WRITE_INPUT_ADDR`).

**Why it's stubbed**: Requires the game-specific input write address
(`0x76E6AC` pointer + offsets `0x18`/`0x24`/`0x2C`/`0x38`).

**What it would take**: Port `DllProcessManager.cpp` (the
`writeGameInput` method) + `DllControllerManager.cpp` +
`DllControllerUtils.hpp` from CCCaster. The controller mapping model
is already implemented in ReCaster (`src/common/controller/mapping.cpp`),
so only the DLL-side consumption + input injection is missing. See
`docs/cccaster-dll-targets.md`.
