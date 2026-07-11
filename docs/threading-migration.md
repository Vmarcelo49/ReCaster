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

- [ ] Add `Snapshot` struct to `session.hpp`:
  - `{ state, error_message, status_message, stats, config, room_code,
       public_ip, local_ip, local_connection_type, remote_connection_type,
       local_name, remote_name, remaining_seconds, room_validation }`
- [ ] Add `Command` variant to `session.cpp` (internal):
  - `StartHost{port, training}`, `StartSmartHost{relay_source, port, training}`,
    `StartJoin{host, port, training}`, `StartSmartJoin{input, relay_source, training}`,
    `StartRelayHost{...}`, `StartRelayJoin{...}`,
    `HostConfirm`, `Cancel`, `Deinit`, `SetLocalName{name}`,
    `SetManualDelay{delay}`, `SetRollback{rollback}`,
    `LookupHostAddresses`, `DetectConnectionType`
- [ ] Add `std::jthread worker_` + `BlockingQueue<Command> commands_`
- [ ] Add `std::mutex state_mutex_` + `Snapshot snapshot_`
- [ ] Refactor public API to async:
  - [ ] `start_host_async(...)`, `start_smart_host_async(...)`, etc.
        (enqueue command, return void)
  - [ ] `host_confirm_async()`, `cancel_async()`, `deinit_async()`
  - [ ] `snapshot()` returns `Snapshot` by value under lock
  - [ ] Keep `set_local_name()`, `set_manual_delay()`, `set_rollback()`
        as enqueue-command (so they're sequenced with start)
- [ ] Worker loop:
  ```cpp
  while (!st.stop_requested()) {
      drain_commands();
      step();
      maybe_update_snapshot();
      std::this_thread::sleep_for(1ms);
  }
  ```
- [ ] Update `waiting_for_peer.cpp`:
  - [ ] Replace `session->step()` call with `session->snapshot()`
  - [ ] Replace all getters with snapshot field reads
- [ ] Update `main_menu.cpp drawWaitingForPeer`:
  - [ ] Use `snapshot().state == Launching` as trigger
  - [ ] `session_->deinit_async()` instead of `session_->deinit()`
- [ ] Update `play_page.cpp`:
  - [ ] `start_smart_host_async(...)` instead of `start_smart_host(...)`
  - [ ] `set_local_name` etc. still work (now enqueued)
- [ ] Update `cli.cpp` if it uses the session directly (CLI path)
- [ ] Build + manual test: host + join between two instances, confirm
      handshake, ping exchange, launch — all identical to before

**Effort:** ~200 LOC. **Risk:** medium (refactors public API).
**Benefit:** UI never blocks on handshake; session state is consistent
snapshots.

### Layer 2 — GameRunner on a dedicated jthread

- [ ] Add `Snapshot` struct to `game_runner.hpp`:
  - `{ is_running, pid, ipc_handshake_done, stop_reason, last_error }`
- [ ] Add `Command` variant (internal):
  - `LaunchOffline{cfg, params}`, `LaunchAfterHandshake{cfg, np_cfg}`,
    `ForceKill`
- [ ] Add `std::jthread worker_` + `BlockingQueue<Command> commands_`
- [ ] Add `std::mutex state_mutex_` + `Snapshot snapshot_`
- [ ] Refactor public API to async:
  - [ ] `launch_offline_async(...)` → returns void, result via snapshot
  - [ ] `launch_after_handshake_async(...)` → same
  - [ ] `force_kill_async()` → same
  - [ ] `snapshot()` returns `Snapshot` by value under lock
- [ ] Worker loop:
  ```cpp
  while (!st.stop_requested()) {
      drain_commands();
      if (launched) update();  // poll is_alive + IPC
      maybe_update_snapshot();
      std::this_thread::sleep_for(16ms);  // ~60fps polling is enough
  }
  ```
- [ ] Update `main_menu.cpp`:
  - [ ] `drawInGame` reads snapshot, no longer calls `update()`
  - [ ] Transition `WaitingForPeer → InGame`:
        on `session.snapshot().state == Launching` →
        `game_runner_.launch_after_handshake_async()` →
        poll snapshot until `is_running` → transition_to(InGame)
  - [ ] `play_page.cpp` offline launch: `launch_offline_async()` →
        poll snapshot → transition_to(InGame)
- [ ] Build + manual test:
  - [ ] Offline Training + Versus launch and run
  - [ ] Netplay host + join launch and run
  - [ ] Force Kill works
  - [ ] Natural game exit detected

**Effort:** ~200 LOC. **Risk:** medium (launch is now async, UI must poll).
**Benefit:** UI never blocks on launch (1-2s CreateProcess + inject).

### Layer 3 — Training while hosting

- [ ] Add `UiState::TrainingWhileHosting` to `ui_state.hpp`
- [ ] Add `MainMenu::drawTrainingWhileHosting()`
- [ ] Add "Launch Training" button to `waiting_for_peer.cpp`:
  - On click: `game_runner_.launch_offline_async(training=true)` +
              `transition_to(TrainingWhileHosting)`
- [ ] `drawTrainingWhileHosting()`:
  - [ ] Read both `session_->snapshot()` and `game_runner_.snapshot()`
  - [ ] Show training PID + Force Kill + "Stop Training" button
  - [ ] Show session room code + ping + status
  - [ ] On `session.snapshot().state == Launching`:
        1. `game_runner_.force_kill_async()`
        2. Poll `game_runner_.snapshot()` until `!is_running`
        3. `game_runner_.launch_after_handshake_async(np_cfg)`
        4. Poll snapshot until `is_running`
        5. `transition_to(InGame)`
  - [ ] On `session.snapshot().state == Failed/Cancelled`:
        `game_runner_.force_kill_async()` + `transition_to(Idle)`
  - [ ] On training natural exit:
        `transition_to(WaitingForPeer)` (session still listening)
- [ ] Extend or disable `kListenTimeoutMs` (currently 1h) when in
      TrainingWhileHosting — user may train longer than 1h
- [ ] Build + manual test:
  - [ ] Host → Launch Training → training runs
  - [ ] Peer connects from another instance → handshake → training killed
  - [ ] Netplay launches automatically → match plays
  - [ ] Stop Training button returns to WaitingForPeer
  - [ ] Cancel from WaitingForPeer with training running kills both

**Effort:** ~200 LOC. **Risk:** low (built on Layers 1+2).
**Benefit:** the actual feature.

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

## Out of scope (for now)

These are explicitly deferred to keep the migration focused:

- **ThreadPool** — only added when a feature needs task parallelism (replay
  compression, 2v2 AI, etc.)
- **`std::shared_mutex`** — only if a second reader of session/game_runner
  snapshots appears
- **DLL-side threading** — the DLL stays single-threaded on the game thread;
  no reason to change
- **Concurrent game instances** — TrainingWhileHosting uses kill-then-relaunch,
  not overlap. Two MBAA.exe running simultaneously is a different (harder)
  feature for later

---

## Progress log

Append a line here when a layer is completed. Reference the worklog.md
entry for details.

- 2026-07-12 — Plan written, awaiting start of Layer 0
- 2026-07-12 — Layer 0 complete: `src/common/concurrency.hpp` (BlockingQueue<T>) + smoke test (19/19 pass). Project builds clean.
