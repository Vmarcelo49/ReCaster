# Threading Migration Plan

Status: **planning** — last updated 2026-07-12

This document tracks the migration of the ReCaster launcher from
single-threaded to a multi-threaded architecture, in preparation for the
"Training while hosting" feature and future parallel features
(spectate, replay takeover, 2v2, discord rich presence, etc.).

It is the canonical reference for the plan. Progress is tracked in the
checkboxes below; design decisions are in the sections that follow.

---

## Motivation

The launcher is currently single-threaded: the ImGui frame loop drives
everything (`session.step()`, `game_runner.update()`, SDL events, render).
This works for the current feature set but blocks the UI during:

- Netplay handshake (already non-blocking via ENet poll, but the API is sync)
- Game launch (`CreateProcessW` + inject + IPC handshake takes 1-2s)
- Future features that need real parallelism (e.g. spectate while playing)

The "Training while hosting" feature (run Training mode while waiting for
a netplay peer, then kill + relaunch in netplay when peer connects) is
the immediate trigger, but the migration is justified on its own merits
as an enabler for the roadmap.

---

## Guiding principles

These come from the C++23 threading guide we reviewed and from the
project's existing conventions.

1. **`std::jthread` everywhere** — auto-join on destruction, cooperative
   cancellation via `std::stop_token`. Never raw `std::thread`.
2. **ENet is single-threaded by contract** — only the dedicated network
   thread touches `ENetHost*`/`ENetPeer*`. No exceptions.
3. **Communicate via queues, not shared state** — workers receive
   commands via `BlockingQueue<Command>` and expose state via a
   `Snapshot` struct read under a mutex. UI never pokes worker internals.
4. **RAII locking** — `std::lock_guard` / `std::unique_lock` only.
   Never manual `.lock()`/`.unlock()`.
5. **`std::mutex` for snapshots** — we have 1 writer + 1 reader today.
   Migrate to `std::shared_mutex` only if a second reader appears
   (e.g. a stats/telemetria thread).
6. **No `ThreadPool` yet** — dedicated workers per subsystem for now
   (network, game runner). A generic pool gets added only when a feature
   needs real task parallelism (e.g. replay compression, 2v2 AI).
7. **Atomics for simple flags** — `std::atomic<bool>` for "is running"
   style flags; `wait`/`notify` for efficient blocking without a cv.
8. **Never `volatile` for cross-thread** — `std::atomic` is the only
   correct tool.

---

## Architecture target

```
[ Main thread (ImGui) ]
  ├── reads session.snapshot()      (under mutex)
  ├── reads game_runner.snapshot()  (under mutex)
  ├── enqueues commands             (BlockingQueue)
  └── renders UI

[ Session worker (jthread) ]
  ├── owns EnetTransport + RelayClient
  ├── loop: process commands → step() → update snapshot
  └── only thread that calls enet_host_service / enet_peer_send

[ GameRunner worker (jthread) ]
  ├── owns WindowsLauncher + IpcServer
  ├── loop: process commands → update() → update snapshot
  └── executes launch / force_kill on this thread (off the UI thread)
```

The DLL side (hook.dll) is unchanged — it already runs single-threaded
on the game's main thread via the hooked callback, and that's correct.

---

## Layers

Each layer is independent and ships on its own. The project must build
and pass manual testing after each layer. Do not start the next layer
until the previous one is solid.

### Layer 0 — Concurrency foundation

- [x] Create `src/common/concurrency.hpp`
  - [x] `BlockingQueue<T>` with `std::mutex` + `std::condition_variable_any`
  - [x] `push(T)`, `try_pop(T&)`, `wait_and_pop(T&, stop_token)` (returns false on stop)
  - [x] `clear()` for shutdown
- [x] Add to `CMakeLists.txt` (header-only, but list it for IDE visibility)
- [x] Smoke test: producer/consumer with 2 jthreads, confirm no leaks/deadlocks
- [x] Build + manual sanity check (no behavior change expected)

**Effort:** ~80 LOC. **Risk:** low. **No behavior change.**
**Status:** ✅ Complete (2026-07-12). Smoke test passes 19/19 checks.

### Layer 1 — NetplaySession on a dedicated jthread

- [x] Add `Snapshot` struct to `session.hpp`:
  - `{ state, error_message, status_message, stats, config, room_code,
       public_ip, local_ip, local_connection_type, remote_connection_type,
       local_name, remote_name, remaining_seconds, room_validation }`
- [x] Add `Command` variant to `session.cpp` (internal):
  - `StartHost{port, training}`, `StartSmartHost{relay_source, port, training}`,
    `StartJoin{host, port, training}`, `StartSmartJoin{input, relay_source, training}`,
    `StartRelayHost{...}`, `StartRelayJoin{...}`,
    `HostConfirm`, `Cancel`, `Deinit`, `SetLocalName{name}`,
    `SetManualDelay{delay}`, `SetRollback{rollback}`,
    `LookupHostAddresses`, `DetectConnectionType`
- [x] Add `std::jthread worker_` + `BlockingQueue<Command> commands_`
- [x] Add `std::mutex state_mutex_` + `Snapshot snapshot_`
- [x] Refactor public API to async:
  - [x] `start_host_async(...)`, `start_smart_host_async(...)`, etc.
        (enqueue command, return void)
  - [x] `host_confirm_async()`, `cancel_async()`, `deinit_async()`
  - [x] `snapshot()` returns `Snapshot` by value under lock
  - [x] Keep `set_local_name()`, `set_manual_delay()`, `set_rollback()`
        as enqueue-command (so they're sequenced with start)
- [x] Worker loop:
  ```cpp
  while (!st.stop_requested()) {
      drain_commands();
      step();
      maybe_update_snapshot();
      std::this_thread::sleep_for(1ms);
  }
  ```
- [x] Update `waiting_for_peer.cpp`:
  - [x] Replace `session->step()` call with `session->snapshot()`
  - [x] Replace all getters with snapshot field reads
- [x] Update `main_menu.cpp drawWaitingForPeer`:
  - [x] Use `snapshot().state == Launching` as trigger
  - [x] `session_->deinit_async()` instead of `session_->deinit()`
- [x] Update `play_page.cpp`:
  - [x] `start_smart_host_async(...)` instead of `start_smart_host(...)`
  - [x] `set_local_name` etc. still work (now enqueued)
- [x] Update `cli.cpp` if it uses the session directly (CLI path)
- [x] Build + manual test: host + join between two instances, confirm
      handshake, ping exchange, launch — all identical to before

**Effort:** ~200 LOC. **Risk:** medium (refactors public API).
**Benefit:** UI never blocks on handshake; session state is consistent
snapshots.
**Status:** ✅ Complete and user-tested (2026-07-12). Host + join via
localhost works end-to-end (connect, handshake, launch, play). One bug
found and fixed during testing: worker_loop was blocking on
wait_and_pop instead of calling step() continuously (commit 832c3e3).

### Layer 2 — GameRunner on a dedicated jthread

- [x] Add `Snapshot` struct to `game_runner.hpp`:
  - `{ is_running, pid, ipc_handshake_done, stop_reason, last_error, launch_in_progress }`
- [x] Add `Command` variant (internal):
  - `LaunchOffline{cfg, params}`, `LaunchAfterHandshake{cfg, np_cfg}`,
    `ForceKill`
- [x] Add `std::jthread worker_` + `BlockingQueue<Command> commands_`
- [x] Add `std::mutex state_mutex_` + `Snapshot snapshot_`
- [x] Refactor public API to async:
  - [x] `launch_offline_async(...)` → returns void, result via snapshot
  - [x] `launch_after_handshake_async(...)` → same
  - [x] `force_kill_async()` → same
  - [x] `snapshot()` returns `Snapshot` by value under lock
- [x] Worker loop:
  ```cpp
  while (!st.stop_requested()) {
      drain_commands();
      if (launched) update();  // poll is_alive + IPC
      maybe_update_snapshot();
      std::this_thread::sleep_for(16ms);  // ~60fps polling is enough
  }
  ```
- [x] Update `main_menu.cpp`:
  - [x] `drawInGame` reads snapshot, no longer calls `update()`
  - [x] Transition `WaitingForPeer → InGame`:
        on `session.snapshot().state == Launching` →
        `game_runner_.launch_after_handshake_async()` →
        poll snapshot until `is_running` → transition_to(InGame)
  - [x] `play_page.cpp` offline launch: `launch_offline_async()` →
        poll snapshot → transition_to(InGame)
- [x] Build + manual test:
  - [x] Offline Training + Versus launch and run
  - [x] Netplay host + join launch and run
  - [x] Force Kill works
  - [x] Natural game exit detected

**Effort:** ~200 LOC. **Risk:** medium (launch is now async, UI must poll).
**Benefit:** UI never blocks on launch (1-2s CreateProcess + inject).
**Status:** ✅ Complete and user-tested (2026-07-12). Offline launch +
netplay launch + Force Kill + natural exit all work. One race condition
found and fixed during testing: launch_in_progress wasn't set
synchronously, causing the UI to fall back to Idle before the worker
picked up the command (commit ad4a5de).

### Layer 3 — Training while hosting

- [x] Add `UiState::TrainingWhileHosting` to `ui_state.hpp`
- [x] Add `MainMenu::drawTrainingWhileHosting()`
- [x] Add "Launch Training" button to `waiting_for_peer.cpp`:
  - On click: `game_runner_.launch_offline_async(training=true)` +
              `transition_to(TrainingWhileHosting)`
- [x] `drawTrainingWhileHosting()`:
  - [x] Read both `session_->snapshot()` and `game_runner_.snapshot()`
  - [x] Show training PID + Force Kill + "Stop Training" button
  - [x] Show session room code + ping + status
  - [x] On `session.snapshot().state == Launching`:
        1. `game_runner_.force_kill_async()`
        2. Poll `game_runner_.snapshot()` until `!is_running`
        3. `game_runner_.launch_after_handshake_async(np_cfg)`
        4. Poll snapshot until `is_running`
        5. `transition_to(InGame)`
  - [x] On `session.snapshot().state == Failed/Cancelled`:
        `game_runner_.force_kill_async()` + `transition_to(Idle)`
  - [x] On training natural exit:
        `transition_to(WaitingForPeer)` (session still listening)
- [ ] Extend or disable `kListenTimeoutMs` (currently 1h) when in
      TrainingWhileHosting — user may train longer than 1h
- [x] Build + manual test:
  - [x] Host → Launch Training → training runs
  - [ ] Peer connects from another instance → handshake → training killed
  - [ ] Netplay launches automatically → match plays
  - [ ] Stop Training button returns to WaitingForPeer
  - [ ] Cancel from WaitingForPeer with training running kills both

**Effort:** ~200 LOC. **Risk:** low (built on Layers 1+2).
**Benefit:** the actual feature.
**Status:** ✅ Build complete (2026-07-12). Awaiting user test.

---

## Risks and mitigations

| Risk | Mitigation |
|------|------------|
| `TerminateProcess` is async — relaunch before full exit causes pipe name conflict | Poll `snapshot().is_running` until false before relaunching; add 100ms sleep as safety margin |
| DLL `DLL_PROCESS_DETACH` under TerminateProcess may not clean hooks cleanly | Verify in testing; if problematic, add a graceful "please exit" IPC message before TerminateProcess |
| Session `kListenTimeoutMs = 1h` may fire during long training sessions | Disable or extend timeout when in TrainingWhileHosting state |
| Race between session worker and game_runner worker when peer connects mid-launch | Both are async with snapshots; UI polls both and sequences the kill-then-relaunch explicitly |
| ENet transport accidentally called from UI thread after refactor | Code review + assert in debug builds that `transport_.poll()` is only called from worker |
| `BlockingQueue` deadlock on shutdown | `wait_and_pop` takes `stop_token` and returns false on stop; workers drain queue on exit |

---

## Out of scope (for launcher migration)

These are explicitly deferred from the launcher migration (Layers 0-3):

- **ThreadPool** — only added when a feature needs task parallelism (replay
  compression, 2v2 AI, etc.)
- **`std::shared_mutex`** — only if a second reader of session/game_runner
  snapshots appears
- **Concurrent game instances** — TrainingWhileHosting uses kill-then-relaunch,
  not overlap. Two MBAA.exe running simultaneously is a different (harder)
  feature for later

---

## Part 2 — DLL-side threading (hook.dll)

**Status: planning** — last updated 2026-07-13

The launcher migration (Layers 0-3 above) is complete. The DLL (hook.dll)
is currently single-threaded: everything runs on the game's main thread via
`callback()` → `frameStep()`. This works for v1 netplay but blocks
spectator support and has performance implications.

### Motivation

The DLL is single-threaded synchronous: `callback()` → `frameStep()` →
ENet `poll()` + spin-lock + rollback + `writeGameInput()`, all on the
game thread (~60fps). This causes:

1. **Spin-lock blocking** — `frameStep()` has a spin-lock
   (`dll_main.cpp:1034-1084`) that blocks the game thread for up to 10s
   waiting for remote input. During this time: spectators can't connect,
   disconnects aren't detected, relay reconnects can't happen, overlay
   doesn't update, window messages aren't pumped.

2. **Rollback rerun blocking** — during rollback rerun (replay of
   multiple frames), ENet packets accumulate without being processed.
   Long reruns (7+ frames) cause packet bursts that pile up.

3. **Spectator broadcast (host-side)** — broadcast round-robin to 15
   spectators takes time in `frameStep()`, causing hitching.

4. **Disconnect detection latency** — `g_connected` is only checked at
   the top of `callback()`. If the peer disconnects mid-frame, the player
   only finds out next frame.

5. **Spectator accept during spin-lock** — impossible without threading.
   This is the hard blocker for spectator mode.

### Architecture target

```
┌─────────────────────────────────────────────────────┐
│  Game Thread (callback() @ 60fps)                   │
│  ├── frameStep() (FSM, rollback, game memory)       │
│  ├── writeGameInput()                               │
│  ├── Overlay rendering (Present hook)                │
│  └── Drain message queue → process netplay msgs      │
├─────────────────────────────────────────────────────┤
│  Network Thread (background loop @ ~100Hz)           │
│  ├── ENet host_service() (send + receive)            │
│  ├── Accept spectator connections                    │
│  ├── Broadcast BothInputs to spectators              │
│  ├── Detect disconnects immediately                  │
│  └── Push received messages to queue                 │
├─────────────────────────────────────────────────────┤
│  Thread-safe message queue (lock-free SPSC)          │
│  Network → Game: PlayerInputs, RngState, etc.        │
│  Game → Network: sendPlayerInputs, sendRngState      │
└─────────────────────────────────────────────────────┘
```

### Key design constraints

1. **D3D9 stays on the game thread** — the Present vtable hook fires on
   the game thread. The network thread never touches D3D9.
2. **Game memory stays on the game thread** — `writeGameInput`,
   `setRngState`, `saveState`, `loadState` all touch `CC_*_ADDR`. The
   network thread never writes to game memory.
3. **SDL2 stays on the game thread** — joystick polling is not
   thread-safe across SDL2's global state.
4. **ENet is single-threaded by contract** — only the network thread
   calls `enet_host_service()` / `enet_peer_send()`. The game thread
   communicates via queues.
5. **`std::jthread` with cooperative cancellation** — matches the
   launcher's approach (guiding principle #1 above).

### Layers

Each layer is independent and ships on its own. The project must build
and pass manual testing after each layer.

#### Layer 4 — Network thread foundation

- [ ] Create `src/dll/netplay/network_thread.hpp` and `.cpp`
  - [ ] `std::jthread` with stop_token
  - [ ] Owns the `ENetHost*` (moved from `connector.cpp`)
  - [ ] Loop: `enet_host_service()` → dispatch events → push to queue
  - [ ] Thread-safe message queue (reuse `BlockingQueue<T>` from
        `src/common/concurrency.hpp`)
- [ ] Refactor `connector.cpp`:
  - [ ] `start()` launches the network thread
  - [ ] `stop()` requests stop + joins
  - [ ] `poll()` replaced by queue drain (non-blocking `try_pop`)
  - [ ] `send()` enqueues to network thread (thread-safe)
- [ ] Add mutex to `NetplayManager` for thread-safe access:
  - [ ] `setInputs` / `getInput` / `getBothInputs` / `setBothInputs`
  - [ ] `setRngState` / `getRngState`
  - [ ] `setState` / `getState`
  - [ ] `setRemoteIndex` / `getRemoteFrame`
- [ ] Update `dll_main.cpp`:
  - [ ] `drainNetplayInbox()` → drain queue (non-blocking)
  - [ ] Remove spin-lock; replace with "try_pop, if empty skip frame"
        or "wait with 1-frame timeout"
  - [ ] `sendPlayerInputs` / `sendRngState` → enqueue to network thread
- [ ] Build + manual test: netplay host + join works identically to before

**Effort:** ~300 LOC. **Risk:** high (touches core netplay path).
**Benefit:** eliminates spin-lock blocking, enables spectator, faster
disconnect detection, smoother rollback.

#### Layer 5 — Spectator host-side

- [ ] Create `src/dll/spec/spectator_manager.hpp` and `.cpp`
  - [ ] Port `DllSpectatorManager.cpp` from CCCaster
  - [ ] Runs on the network thread (accept + broadcast)
  - [ ] No Timer/EventManager — use `GetTickCount()`
  - [ ] No Socket* — use ENet peer IDs
  - [ ] No mutexes for spectator state (network thread owns it)
- [ ] Integrate `stepSpectators()` into the network thread loop
- [ ] Accept spectator connections (ENet connect event on network thread)
- [ ] Send SpectateConfig + InitialGameState + RngState on accept
- [ ] Broadcast `BothInputs` round-robin (throttled by spectator count)

**Effort:** ~200 LOC. **Risk:** medium (new code, but isolated).
**Benefit:** host can accept spectators without blocking the game.

#### Layer 6 — Spectator client-side

- [ ] Create `src/dll/spec/spectate_client.hpp` and `.cpp`
  - [ ] Receive BothInputs → push to game thread queue
  - [ ] Receive RngState → push to game thread queue
  - [ ] Receive MenuIndex → push to game thread queue
- [ ] Integrate into FSM:
  - [ ] `AutoCharaSelect` state: auto-navigate chara-select if late-join
  - [ ] `getInput()` returns 0 (spectator doesn't play)
  - [ ] No resend, no rollback (spectator just replays)
- [ ] Fast-forward / hard-sync controls (toggle with hotkey)

**Effort:** ~150 LOC. **Risk:** medium.
**Benefit:** spectator can watch matches in real-time.

### Risks and mitigations (DLL-side)

| Risk | Mitigation |
|------|------------|
| Race condition in NetplayManager | Granular mutexes (one per critical field group, not global) |
| Deadlock between game thread and network thread | Lock ordering: network thread always acquires locks before pushing to queue; game thread always drains queue before acquiring locks |
| D3D9 not thread-safe | Network thread never touches D3D9 — only game thread renders |
| Game memory not thread-safe | Network thread never writes to `CC_*_ADDR` — only game thread does |
| SDL2 not thread-safe for joysticks | Network thread never touches SDL — only game thread polls joysticks |
| Debugging multi-threaded DLL | Logs already have thread_id; use ThreadSanitizer on Linux/Wine builds |
| ENet packet ordering with queue | Queue is FIFO (SPSC); ENet reliable channels preserve order; unreliable channels don't need queue ordering |
| `DLL_PROCESS_DETACH` during active network thread | `stop()` called in `deinitialize()` before any hooks are removed; jthread auto-joins |

### What does NOT need threading

| Component | Why it stays on the game thread |
|---|---|
| D3D9 rendering (Present hook) | D3D9 is not thread-safe; the Present hook fires on the game thread |
| Game memory I/O (`writeGameInput`, `setRngState`) | Game memory is not thread-safe; all `CC_*_ADDR` writes must be on the game thread |
| SDL2 joystick polling | SDL2 global state is not thread-safe |
| Overlay rendering | Uses D3D9 — must be on the game thread |
| Keymapper | Uses SDL2 + D3D9 — must be on the game thread |
| Rollback save/load state | Touches game memory — must be on the game thread |
| ASM patches | Game code — must be on the game thread |

### Estimated effort

| Layer | LOC | Sessions | Depends on |
|---|---|---|---|
| 4 — Network thread foundation | ~300 | 2 | Layers 0-3 (launcher, done) |
| 5 — Spectator host-side | ~200 | 1 | Layer 4 |
| 6 — Spectator client-side | ~150 | 1 | Layer 4 + 5 |
| **Total** | **~650** | **3-4** | |

Note: spectator protocol (SpectateConfig message + decoder) is ~40 LOC
and can be done as part of Layer 5.

---

## Progress log

Append a line here when a layer is completed. Reference the worklog.md
entry for details.

- 2026-07-12 — Plan written, awaiting start of Layer 0
- 2026-07-12 — Layer 0 complete: `src/common/concurrency.hpp` (BlockingQueue<T>) + smoke test (19/19 pass). Project builds clean.
- 2026-07-12 — Layer 1 complete: NetplaySession refactored to worker jthread with async API + snapshot. All callers updated (waiting_for_peer, main_menu, play_page, cli). Build passes, no warnings.
- 2026-07-12 — Layer 1 user-tested: host + join via localhost works end-to-end. Fixed worker_loop blocking bug (commit 832c3e3).
- 2026-07-12 — Layer 2 build complete: GameRunner refactored to worker jthread with async API + snapshot. All callers updated (main_menu, play_page, cli). Awaiting user test.
- 2026-07-12 — Layer 2 user-tested: offline launch + netplay launch + Force Kill + natural exit all work. Fixed race condition where launch_in_progress wasn't set synchronously (commit ad4a5de).
- 2026-07-12 — Layer 3 build complete: Training while hosting feature implemented. New UiState::TrainingWhileHosting, "Launch Training" button in WaitingForPeer, drawTrainingWhileHosting with kill-then-relaunch transition. Awaiting user test.
