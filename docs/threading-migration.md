# Threading Migration Plan

Status: **Parte 1 completa · Parte 2 — Layer 4 + Phase B + Phase C implementadas (spectator re-enabled, pending runtime validation)** — last updated 2026-07-15

This document tracks the migration of the ReCaster launcher from
single-threaded to a multi-threaded architecture (Part 1 — completed),
and the migration of the DLL `hook.dll` from single-threaded to a
network-thread + game-thread architecture (Part 2 — implemented,
runtime-validated for the netplay regression path; spectator mode was
re-enabled at the network layer post-`027d9ee` and awaits runtime
validation under Wine+MBAACC.exe).

It is the canonical reference for the plan. Progress is tracked in the
checkboxes below; design decisions are in the sections that follow.

**State at last update (post-`3fd525e`, 2026-07-15):**
- Layers 0-3 (launcher): ✅ complete and user-validated.
- Layer 4 (DLL network thread foundation): ✅ implemented, debug-build
  thread-affinity asserts in place (TSan not viable on MinGW — see
  subtask 4.9), core netplay regression PASSED on localhost.
- Phase B (speculative rollback, `implementing-real-rollback.md`):
  ✅ B1-B4 implemented. Spin-lock stays as a safety net but no longer
  blocks on `netplay::poll()` — the network thread delivers packets to
  inboxes in the background. Full removal of the spin-lock is Phase 1
  of `implementing-real-rollback.md` (still pending).
- Phase C / spectator (Layers 5-6, `spectator-plan.md`): ✅ classes
  exist, wired through launcher/GUI/session/dll_main/connector/
  NetworkThread/CMakeLists, AND re-enabled at the network layer.
  The two DISABLERS that previously kept it turned off (CONNECT
  handler + peerCapacity=2) were fixed post-`027d9ee`. Build clean
  (zero warnings, zero errors). **Runtime validation under
  Wine+MBAACC.exe is the only remaining gate** — see "Layer 5 status"
  below for the test matrix.

---

## Motivation

The launcher was single-threaded: the ImGui frame loop drove
everything (`session.step()`, `game_runner.update()`, SDL events, render).
This worked for the initial feature set but blocked the UI during:

- Netplay handshake (already non-blocking via ENet poll, but the API was sync)
- Game launch (`CreateProcessW` + inject + IPC handshake takes 1-2s)
- Future features that need real parallelism (e.g. spectate while playing)

The "Training while hosting" feature (run Training mode while waiting for
a netplay peer, then kill + relaunch in netplay when peer connects) was
the immediate trigger, but the migration is justified on its own merits
as an enabler for the roadmap.

For Part 2 (DLL-side), the motivation is different: the DLL is currently
single-threaded synchronous — `callback()` → `frameStep()` → ENet
`poll()` + spin-lock + rollback + `writeGameInput()`, all on the game
thread (~60fps). This blocks spectator support, makes disconnect detection
latent, and is the hard prerequisite for the speculative rollback
migration (`implementing-real-rollback.md`).

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

### Part 1 — Launcher (complete)

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

### Part 2 — DLL `hook.dll` (in progress)

```
┌─────────────────────────────────────────────────────┐
│  Game Thread (callback() @ 60fps)                   │
│  ├── frameStep() (FSM, rollback, game memory)       │
│  ├── writeGameInput()                               │
│  ├── Overlay rendering (Present hook)                │
│  └── Drain message queue → process netplay msgs      │
├─────────────────────────────────────────────────────┤
│  Network Thread (jthread, background loop @ ~100Hz)  │
│  ├── owns ENetHost* + ENetPeer*                      │
│  ├── enet_host_service() (send + receive)            │
│  ├── Accept spectator connections (Layer 5)          │
│  ├── Broadcast BothInputs to spectators (Layer 5)    │
│  ├── Detect disconnects immediately                  │
│  ├── Network simulator (lag/jitter/loss for testing) │
│  └── Push received messages to inbox queues          │
├─────────────────────────────────────────────────────┤
│  Thread-safe message queues (BlockingQueue<T> × 5)   │
│  Network → Game: PlayerInputs, TransitionIndex,      │
│                 MenuIndex, RngState, SyncHash         │
│  Game → Network: sendPlayerInputs, sendRngState, ... │
│  (enqueued via NetworkThread::send_*)                │
└─────────────────────────────────────────────────────┘
```

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

- [x] Add `SessionSnapshot` struct to `session.hpp` (named `SessionSnapshot`,
      not `Snapshot` — verified in session.hpp:55-66):
  - `{ state, error_message, status_message, stats, config, room_code,
       public_ip, local_ip, remaining_seconds, room_validation }`
      Note: `local_name` / `remote_name` live inside the embedded
      `NetplayConfig config` field, not as direct snapshot fields.
      `local_connection_type` / `remote_connection_type` are not in
      the snapshot (those are computed at handshake time and exposed
      through `stats`).
- [x] Add `Command` variant to `session.cpp` (internal):
  - `StartHost{port, training}`, `StartSmartHost{relay_source, port, training}`,
    `StartJoin{host, port, training}`, `StartSmartJoin{input, relay_source, training}`,
    `StartRelayHost{...}`, `StartRelayJoin{...}`,
    `StartSpectate{host, port}`, `StartRelaySpectate{relay_source, room_code}`,
    `HostConfirm`, `Cancel`, `Deinit`, `SetLocalName{name}`,
    `SetManualDelay{delay}`, `SetRollback{rollback}`,
    `LookupHostAddresses`, `DetectConnectionType`
      (The `StartSpectate` + `StartRelaySpectate` commands were added
      with Phase C / spectator mode in commit `027d9ee`.)
- [x] Add `std::jthread worker_` + `BlockingQueue<Command> commands_`
- [x] Add `std::mutex state_mutex_` + `SessionSnapshot snapshot_`
- [x] Refactor public API to async:
  - [x] `start_host_async(...)`, `start_smart_host_async(...)`, etc.
        (enqueue command, return void)
  - [x] `host_confirm_async()`, `cancel_async()`, `deinit_async()`
  - [x] `start_spectate_async(host, port)` +
        `start_relay_spectate_async(relay_source, room_code)`
        (added in Phase C — session.hpp:165, 170)
  - [x] `snapshot()` returns `SessionSnapshot` by value under lock
  - [x] Keep `set_local_name()`, `set_manual_delay()`, `set_rollback()`
        as enqueue-command (so they're sequenced with start)
- [x] Worker loop (verified in session.hpp:194-217):
  ```cpp
  while (!st.stop_requested()) {
      drain_commands();
      step();
      publish_snapshot();   // NOT maybe_update_snapshot()
      std::this_thread::sleep_for(8ms);
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
`wait_and_pop` instead of calling `step()` continuously (commit 832c3e3).

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
**Status:** ✅ Build complete (2026-07-12). Awaiting user test of the
"peer connects mid-training" path. The `kListenTimeoutMs` extension is
a small follow-up that can be done anytime.

---

## Risks and mitigations (Part 1 — launcher)

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

**Status: Layer 4 + Phase B + Phase C implementadas (spectator desabilitado)** — last updated 2026-07-15

The launcher migration (Layers 0-3 above) is complete. The DLL (hook.dll)
migration is now also implemented as of commit `027d9ee` (2026-07-14):
a dedicated network jthread owns the `ENetHost*`, `BlockingQueue<T>`
inboxes carry messages from the network thread to the game thread, and
`NetplayManager` is guarded by a global mutex with the `*Locked`
convention. Phase B (speculative rollback, B1-B4) is implemented on
top of Layer 4 — see `implementing-real-rollback.md`. Phase C (spectator
mode) is fully wired but two DISABLERS in `network_thread.cpp` keep it
turned off at the network layer pending Wine regression investigation —
see Layer 5 below.

### Motivation

Before Layer 4, the DLL was single-threaded synchronous:
`callback()` → `frameStep()` → ENet `poll()` + spin-lock + rollback +
`writeGameInput()`, all on the game thread (~60fps). This caused:

1. **Spin-lock blocking** — `frameStep()` has a spin-lock
   (`dll_main.cpp:1315-1390`) that blocks the game thread for up to 10s
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

6. **Blocks speculative rollback** — `implementing-real-rollback.md`
   requires a dedicated network thread + lock-free queue as Phase 0
   prerequisite. Layer 4 IS that prerequisite.

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

---

### Design decisions — Layer 4 (resolved 2026-07-14)

Three design questions were raised and resolved before implementation.
The decisions below are binding for Layer 4. Each may be revisited when
Phase B (speculative rollback, `implementing-real-rollback.md`) shows
real profiling data.

#### Decision 1 — Mutex granularity: **global mutex now, refine in Phase B**

`NetplayManager` gets a single `std::mutex _mutex` protecting all
mutable state. No fine-grained locking yet.

**Rationale:**
- Phase B will reshape `NetplayManager` state (new fields for prediction
  history, rollback window tracking, etc.). Designing fine-grained locks
  now means guessing the granularity twice — once without contention
  data, again when Phase B arrives. Pay the design cost once, with full
  information.
- A single uncontended `std::mutex` fast path is ~tens of nanoseconds
  per acquisition. Even at several thousand acquisitions per frame
  (game thread's `getInput`/`getState`/`isInRollback` reads + network
  thread's `setInputs`/`setRngState` writes), this stays well under the
  16.6ms frame budget.
- Matches the pattern used in Part 1 (`session.cpp`, `game_runner.cpp`)
  — one mutex per worker, validated under load.

**Migrate to fine-grained (Option B: 3 mutexes for `_inputs` / `_state`
/ `_rngStates`) only if Phase B profiling shows contention.**

##### Convention: `*Locked` suffix

The biggest implementation risk with a global mutex is **self-deadlock**:
a public function calling another public function, both trying to
acquire the same non-recursive `std::mutex`. With ~860 LOC and a rich
call graph, this is easy to slip in.

**Binding convention:**
- **Public functions** (`getInput`, `isInRollback`, `setInputs`, ...):
  acquire `std::lock_guard<std::mutex> lock(_mutex)` on entry, then
  dispatch to the `*Locked` private helper. Never call another public
  function from inside a public function.
- **Private helpers** (`getInputLocked`, `isInRollbackLocked`,
  `setInputsLocked`, ...): assume the caller already holds `_mutex`.
  No lock acquisition inside. May call other `*Locked` helpers freely.
- The `*Locked` suffix is **mandatory** for any private function that
  assumes the lock is held. Code review rejects PRs that violate this.

**Example:**
```cpp
class NetplayManager {
    mutable std::mutex _mutex;

public:
    // Public entry point — acquires lock, dispatches.
    uint16_t getInput(uint8_t player) {
        std::lock_guard<std::mutex> lock(_mutex);
        return getInputLocked(player);
    }
    bool isInRollback() const {
        std::lock_guard<std::mutex> lock(_mutex);
        return isInRollbackLocked();
    }

private:
    // Internal — caller must hold _mutex.
    uint16_t getInputLocked(uint8_t player);
    bool isInRollbackLocked() const;

    // Public getInput() CANNOT call public isInRollback() —
    // that would re-enter _mutex and deadlock.
    // It calls isInRollbackLocked() instead.
};
```

#### Decision 2 — Queue format: **5 separate `BlockingQueue<T>`**

Keep the 5 inbox queues that exist today in `connector.cpp:85-89`, but
promote each from `std::queue<T>` (no lock) to
`caster::common::concurrency::BlockingQueue<T>` (thread-safe).

```
BlockingQueue<PlayerInputs>  inboxPlayerInputs;
BlockingQueue<uint32_t>      inboxTransitionIndex;
BlockingQueue<MenuIndex>     inboxMenuIndex;
BlockingQueue<RngState>      inboxRngState;
BlockingQueue<SyncHash>      inboxSyncHash;
```

**Rationale:**
- **Zero change to `drainNetplayInbox()`** in `dll_main.cpp:725-761` —
  the function keeps its 5 `while (auto x = recvX())` loops unchanged,
  just with `BlockingQueue::try_pop` underneath instead of
  `std::queue::pop`. This is the critical rollback FSM hot path;
  minimizing churn here reduces regression risk.
- The current single-threaded code already drains all of one type
  before moving to the next (no cross-type ordering). The 1653+ rollback
  validation run worked under this model. Switching to a single
  `BlockingQueue<std::variant<...>>` would *add* cross-type ordering as
  a new property the system doesn't currently rely on — and require
  rewriting `drainNetplayInbox()` with `std::visit`.
- Each queue is naturally SPSC (network thread = producer, game thread =
  consumer). Easy to reason about, easy to prove correct.
- The launcher's `session.cpp` / `game_runner.cpp` use a single
  `BlockingQueue<std::variant<Command>>` — that's the right pattern when
  there are 10+ command types and global ordering matters. With 5
  message types and no ordering requirement, 5 queues is simpler.
- **Migrate to `std::variant` only if message type count grows past
  ~10, or if a future feature requires global cross-type ordering.**

#### Decision 3 — Network simulator: **isolated `NetworkSimulator` class, built now**

The `CASTER_SIM_LAG_MS` / `CASTER_SIM_JITTER_MS` / `CASTER_SIM_LOSS_PCT`
simulator that lives in `connector.cpp:52-77` today is **essential**
for testing rollback without real network latency. It was used to
validate the 1653+ rollback run documented in `port-status.md`.

Create `src/dll/netplay/network_simulator.{hpp,cpp}` as a standalone
class that the `NetworkThread` owns. Move the simulator logic out of
`connector.cpp` into this class.

**Rationale for doing it now (not as follow-up):**
- The `poll()` and receive path are being rewritten for the network
  thread anyway. The marginal cost of encapsulating the simulator while
  already touching this code is lower than doing it as a separate
  migration later.
- Phase B (speculative rollback) is the highest-risk milestone in the
  roadmap. Keeping the simulator available is essential for reproducing
  desync scenarios locally. Cutting it would mean debugging desyncs with
  `tc netem` / Clumsy / 2 machines — much slower feedback loop.

**Implementation requirements:**
- **`std::mt19937` member, seeded once** by `std::random_device` at
  `NetworkThread` construction. `std::rand()` is not thread-safe and
  must not be used.
- **Optional `CASTER_SIM_SEED` env var** for reproducible test runs —
  when set, the RNG is seeded from this value instead of
  `std::random_device`.
- **Env vars read in `NetworkThread::start()` before the loop begins**,
  not lazily on first packet. This guarantees the simulator config is
  stable before any packet flows.
- **Applies only to `PlayerInputs`** (UNRELIABLE per-frame), as today.
  Control messages (TransitionIndex, MenuIndex, RngState, SyncHash)
  are always delivered immediately — they use RELIABLE channels and
  delaying them would break handshake/state-sync logic, not test
  rollback.

#### Decision 4 — ThreadSanitizer: investigate feasibility, fallback to manual audit

ThreadSanitizer is the only tool that catches data races the 1653+ rollback
validation can't (that run was single-threaded and proves nothing about
Layer 4's new races).

**Caveats:**
- MinGW-w64 GCC `-fsanitize=thread` has historical bugs on Windows.
- Clang-MinGW (used for the overlay vtable swap) has better support but
  is not first-class.
- MSVC TSan is x64-only and the project is i686 (MBAACC is 32-bit).
- TSan inside Wine is theoretical but impractical.

**Plan:**
- Subtask 4.9: try enabling TSan via Clang-MinGW in debug builds. If it
  works, run the Layer 4 validation host+join with
  `CASTER_SIM_LAG_MS=100` under TSan.
- **Fallback if TSan doesn't work:** rely on rigorous code review of
  the `*Locked` convention + the existing `SyncHash` desync detection
  as the safety net. Add `assert`s in debug builds that
  `enet_host_service` is only called from the network thread.

---

### Layer 4 — Network thread foundation

Estimated effort: **~350-450 LOC, 3-4 sessions.** Risk: high (touches
core netplay path). Benefit: eliminates spin-lock blocking (when Phase B
removes the spin-lock), enables spectator, faster disconnect detection,
smoother rollback, unblocks speculative rollback.

#### Subtask 4.1 — `NetworkThread` scaffolding

- [x] Create `src/dll/netplay/network_thread.hpp` and `.cpp`
  - [x] `std::jthread` with `stop_token`
  - [x] Owns the `ENetHost*` (moved from `connector.cpp`)
  - [x] `start(cfg)` reads env vars, initializes ENet, launches jthread
  - [x] `stop()` requests stop + joins + destroys ENetHost
  - [x] Loop skeleton: `enet_host_service(host, 10ms)` → dispatch events
        → push to inbox queues
  - [x] Add to `CMakeLists.txt` hook target
- [x] Build check — no behavior change yet (NetworkThread exists but
      isn't wired into `connector.cpp`)

#### Subtask 4.2 — `NetworkSimulator` class

- [x] Create `src/dll/netplay/network_simulator.hpp` and `.cpp`
  - [x] Class with `SimConfig` (lag_ms, jitter_ms, loss_pct, enabled)
  - [x] `std::mt19937 rng_` member, seeded by `std::random_device` or
        `CASTER_SIM_SEED` env var
  - [x] `bool shouldDrop()` — returns true if loss_pct check fails
  - [x] `std::optional<std::chrono::steady_clock::time_point> maybeDelay()`
        — returns nullopt for immediate delivery, or the delivery
        timestamp for delayed delivery
  - [x] `void deliverExpired(std::deque<PlayerInputs>& outbox)` —
        pushes messages whose delay has elapsed
  - [x] Internal `std::deque<DelayedMessage> delayQueue_`
  - [x] Add to `CMakeLists.txt` hook target
- [x] Build check — class exists, integrated as NetworkThread member

#### Subtask 4.3 — Move state into `NetworkThread`

- [x] Move `g_host`, `g_peer`, `g_connected`, `g_isHost`, `g_localPort`,
      `g_peerAddr`, `g_peerPort` from `connector.cpp` anonymous namespace
      into `NetworkThread` private members
- [x] Move the 5 `g_inbox*` `std::queue<T>` → `BlockingQueue<T>` members
      of `NetworkThread`
- [x] Move `g_delayQueue` + `deliverExpiredDelayed()` into
      `NetworkSimulator`
- [x] Move `sendPacket()` into `NetworkThread::loop()` (outbox drain)
- [x] Network thread loop implements the full receive/send/sim path:
  ```
  while (!st.stop_requested()) {
      sim_.deliverExpired(inboxPlayerInputs_);
      ENetEvent ev;
      while (enet_host_service(host_, &ev, 10) > 0) {
          // dispatch: on RECEIVE → decode → route to matching inbox
          // on PlayerInputs: sim_.shouldDrop() / sim_.maybeDelay()
          // on CONNECT/DISCONNECT: update connected_ atomic
      }
      // drain outbox_ → enet_peer_send + enet_host_flush
  }
  ```

#### Subtask 4.4 — `connector.cpp` becomes facade

- [x] `connector.cpp` keeps the same public API (`start`, `poll`,
      `shutdown`, `connected`, `isHost`, `sendPlayerInputs`, `recv*`)
      but delegates to a global `NetworkThread*` instance
- [x] `start(cfg)` → `g_networkThread.start(cfg)`
- [x] `shutdown()` → `g_networkThread.stop()` + delete
- [x] `poll()` → no-op (kept for source compatibility; comment explains)
- [x] `sendPlayerInputs(pi)` → `g_networkThread.enqueueOutbox({pi.serialize(), reliable=false})`
- [x] `recvPlayerInputs()` → `g_networkThread.inboxPlayerInputs().try_pop()`
- [x] Same pattern for the other 4 send/recv pairs
- [x] Build + manual test: behavior is **identical** to before
      (validated in subtask 4.8 — host+join via localhost, end-to-end)

#### Subtask 4.5 — `NetplayManager` mutex

- [x] Add `mutable std::mutex _mutex;` to `NetplayManager`
- [x] Rename all private helpers that assume lock-held to `*Locked`
      suffix (e.g. `getInGameInput` → `getInGameInputLocked`,
      `getRawInput` → `getRawInputLocked`, `isInRollback` const-eval →
      `isInRollbackLocked`)
- [x] Wrap every public function with `std::lock_guard<std::mutex> lock(_mutex);`
      + dispatch to `*Locked` variant
- [x] Audit call graph: no public function calls another public
      function (would self-deadlock). All internal calls go through
      `*Locked` helpers.
- [x] `RollbackManager` is `friend` — its direct field access in
      `loadState`/`saveState` now acquires `_mutex` via
      `std::lock_guard<std::mutex> netManLock(netMan._mutex);`
      + `SCOPED_NETMAN_MUTEX_HELD();` (rollback_manager.cpp:113,189).
      Done as the subtask 4.5 follow-up commit before subtask 4.8
      manual test.
- [x] Build + manual test: host+join still works, no deadlock
      (validated in subtask 4.8 — host+join via localhost, end-to-end)

#### Subtask 4.6 — `dll_main.cpp` updates

- [x] Remove the `caster::dll::netplay::poll();` call at the top of
      `frameStep()` and inside the spin-lock gate. The network thread
      polls itself now.
- [x] Keep `drainNetplayInbox()` non-blocking — it does `try_pop`
      on 5 inboxes (PlayerInputs, TransitionIndex, MenuIndex, RngState,
      SyncHash) plus 3 spectator-only inboxes (SpectateConfig,
      InitialGameState, BothInputs) drained when `g_isSpectator`.
      The spectator-side drain is wired in dll_main.cpp:802-812 but
      has no effect until Layer 5 is re-enabled.
- [x] **Spin-lock stays.** It will be removed in Phase B (speculative
      rollback). For Layer 4, the spin-lock no longer calls
      `netplay::poll()` internally — it just does `drainNetplayInbox()`
      + `Sleep(1)` + retry. The network thread keeps delivering packets
      to the inboxes in the background while the spin-lock blocks.
- [x] Update comments to reflect that ENet polling now happens on the
      network thread.

#### Subtask 4.7 — `DLL_PROCESS_DETACH` ordering

- [x] In `dll_main.cpp` `DLL_PROCESS_DETACH` case, the order is now
      documented and verified:
  1. `g_running.store(false)` — already done
  2. `caster::dll::netplay::shutdown()` — calls `NetworkThread::stop()`
     which requests stop on the jthread, joins it, THEN destroys the
     ENetHost. **Critical: the join must happen before ENetHost
     destruction** or the network thread may touch freed memory.
  3. `SDL_JoystickClose` — game thread only, safe
  4. `caster::dll::dll_hacks::deinitialize()` — unhooks game code
- [ ] Add an assertion in debug builds that the network thread is
      stopped before `deinitialize()` returns (deferred — low priority,
      the explicit shutdown() ordering is already correct)
- [x] Build + manual test: graceful exit (close game window) and
      force-kill (Force Kill button in launcher) both clean up without
      crash or hang (validated in subtask 4.8)

#### Subtask 4.8 — Build + manual test (Layer 4 acceptance)

- [x] Build clean with no warnings (subtask 4.8a — MinGW cross-compile)
- [x] Manual test matrix:
  - [x] Host + join via localhost, no simulator — match plays at 60fps,
        rollback fires correctly, no desync. **User-validated 2026-07-14:
        two caster.exe instances on localhost, online netplay works
        end-to-end.**
  - [ ] Host + join via localhost, `CASTER_SIM_LAG_MS=100` — match
        plays, rollback fires more often, no desync. **This is the
        regression test for the 1653+ rollback validation.**
  - [ ] Host + join via localhost, `CASTER_SIM_LAG_MS=200
        CASTER_SIM_JITTER_MS=20 CASTER_SIM_LOSS_PCT=5` — extreme
        conditions, match stays in sync via SyncHash
  - [ ] Disconnect mid-match (close one side's window) — other side
        detects within 1 frame (not 10s spin-lock timeout) and shows
        "Opponent disconnected"
  - [ ] Force Kill from launcher — `DLL_PROCESS_DETACH` cleans up
        without hang
- [ ] Compare FPS / rollback count / SyncHash stability to pre-Layer-4
      baseline. Any regression blocks merge.

**Status:** Core netplay regression PASSED. Remaining items are
stress-test variants that exercise the new threading paths harder.

#### Subtask 4.9 — ThreadSanitizer feasibility

**Status: TSan not viable — fallback implemented and validated.**

Investigation results (2026-07-14, on Cachyos Linux with both toolchains):

| Toolchain | TSan support |
|---|---|
| Clang-MinGW i686 (`/opt/llvm-mingw/bin/i686-w64-mingw32-clang++` 22.1.7) | ❌ `unsupported option '-fsanitize=thread' for target 'i686-w64-windows-gnu'` |
| Clang-MinGW x86_64 (`/opt/llvm-mingw/bin/x86_64-w64-mingw32-clang++` 22.1.7) | ❌ `unsupported option '-fsanitize=thread' for target 'x86_64-w64-windows-gnu'` |
| GCC-MinGW i686 (`i686-w64-mingw32-g++` 16.1.0) | ❌ compiles but linker fails: `cannot find -ltsan` (no runtime lib shipped with MinGW) |
| Clang Linux native (control) | ✅ works — confirms the test is valid |

**Root cause:** ThreadSanitizer requires a per-platform runtime library.
LLVM ships TSan runtime for Linux, macOS, FreeBSD, Android, NetBSD, and
Fuchsia — but **not for Windows MinGW targets** (only MSVC x64 is
supported, and the project is i686). GCC MinGW doesn't ship a TSan
runtime at all. There is no viable path to TSan for this toolchain.

**Fallback implemented:** `src/dll/netplay/thread_affinity.hpp` — a
header-only debug-build assertion layer that catches the most dangerous
threading mistakes TSan would have caught:

- **`check_network_thread_only(fn)`** — asserts the calling thread is
  the NetworkThread's jthread. Used at the top of the loop before
  `enet_host_service()`. Catches accidental ENet calls from the game
  thread.
- **`check_not_holding_netman_mutex(fn)`** — asserts the calling
  thread is NOT holding `NetplayManager::_mutex`. Used at the top of
  every `netplay::send*` function. Catches the deadlock pattern of
  "blocking on outbox enqueue while holding the FSM lock".
- **`SCOPED_NETMAN_MUTEX_HELD()`** macro — placed right after every
  `std::lock_guard` in `NetplayManager`'s public wrappers. Increments
  a `thread_local` counter that `check_not_holding_netman_mutex` reads.
  In Release (NDEBUG), expands to `((void)0)`.
- **`set_current_thread_as_network_thread()`** — called at the top of
  `NetworkThread::loop()` to register the jthread's TID.
- **`clear_network_thread()`** — called in `NetworkThread::stop()` after
  join to prevent TID-recycling false positives.

All asserts are NO-OPs in Release builds. In Debug builds, they call
`std::abort()` on violation with a clear log message via
`caster::common::logger::err`.

**Validation:**
- [x] Release build (`bash scripts/build.sh`) — clean, no warnings
- [x] Debug build (`CASTER_BUILD_TYPE=Debug cmake ...`) — clean, no
      warnings. Confirms the `SCOPED_NETMAN_MUTEX_HELD` macro and the
      `thread_local` scope tracker compile and link under MinGW.
- [x] `thread_affinity.hpp` syntax-checked in both Release (NDEBUG) and
      Debug modes

**What this fallback DOES catch (same as TSan would):**
- ENet I/O from the wrong thread
- Blocking operations while holding the FSM mutex
- NetworkThread TID mismatches

**What this fallback DOES NOT catch (TSan would have):**
- Data races on individual fields that don't go through the asserted
  functions (e.g. a future `RollbackManager` direct field access that
  forgets to acquire `_mutex`)
- Memory ordering bugs (we use `acquire/release` consistently, but TSan
  would catch a missed `atomic_thread_fence`)
- races in the game's own memory (which we don't control anyway)

The `*Locked` convention + `SyncHash` desync detection + this assertion
layer together provide a reasonable safety net for Layer 4. The next
opportunity for full TSan coverage would be porting to MSVC x64 (which
has TSan support) — but that requires the project to migrate off MinGW
entirely, which is out of scope.

---

### Layer 5 — Spectator host-side

**Status: ✅ implementado, wired, e re-enabled no network thread (post-`3fd525e`). Pendente apenas validação runtime em Wine+MBAACC.exe.**

- [x] Create `src/dll/spec/spectator_manager.hpp` and `.cpp`
  - [x] Port `DllSpectatorManager.cpp` from CCCaster
  - [x] Runs on the network thread (accept + broadcast)
  - [x] No Timer/EventManager — use `GetTickCount()`
  - [x] No Socket* — use ENet peer IDs
  - [x] No mutexes for spectator state (network thread owns it) — note:
        the implementation has a `mutable std::mutex _outMutex` that
        guards both the outbox queue AND the spectator state, because
        `frameStepSpectators()` is called from the game thread while
        `onSpectatorConnect/Disconnect` + `tryPopOut` run on the
        network thread. With `MAX_ROOT_SPECTATORS=1` contention is
        non-existent, but the "no mutexes" claim from the original
        plan was revised during implementation.
- [x] Integrate `stepSpectators()` into the network thread loop
      (`NetworkThread::loop` calls `spectatorMgr_->step()` + drains
      the outbox every iteration)
- [x] Accept spectator connections (ENet connect event on network
      thread) — **RE-ENABLED post-`3fd525e`**: the CONNECT handler
      in `network_thread.cpp:251-309` now distinguishes opponent
      (first inbound CONNECT on host, or any CONNECT on client) from
      spectator (subsequent inbound CONNECT on host when `peer_` is
      already set). Spectator CONNECTs are delegated to
      `spectatorMgr_->onSpectatorConnect(peer)`. See "Layer 5
      re-enablement" below for details.
- [x] Send SpectateConfig + InitialGameState + RngState on accept
      (in `SpectatorManager::promotePending`)
- [x] Broadcast `BothInputs` round-robin (throttled by spectator
      count, in `SpectatorManager::frameStepSpectators`)
- [x] Wire dll_main.cpp `frameStep` to call `promoteAllPending()` +
      `frameStepSpectators()` when host with spectators/pending
      (dll_main.cpp:1051-1061, with a HOTFIX throttle that only
      runs when `numSpectators() > 0 || numPending() > 0`)
- [x] DISCONNECT handler delegates spectator disconnects to
      `spectatorMgr_->onSpectatorDisconnect(peer)` (post-`3fd525e`).

**Effort:** ~260 LOC real (was ~200 estimated; larger because of
the outbox queue + threading comments). **Risk:** medium.

#### Layer 5 re-enablement (post-`3fd525e`, 2026-07-15)

The two DISABLERS that previously kept spectator mode off at the
network layer (commit `027d9ee`, see progress log) have been removed:

**Fix 1 — CONNECT handler now distinguishes opponent from spectator**
(`network_thread.cpp:251-309`):
```cpp
case ENET_EVENT_TYPE_CONNECT: {
    const bool is_opponent = !isHost_ || peer_ == nullptr;
    if (is_opponent) {
        peer_ = ev.peer;
        connected_.store(true, std::memory_order_release);
        // logs "opponent CONNECTED from ..."
    } else {
        // Spectator connection.
        if (spectatorMgr_) {
            spectatorMgr_->onSpectatorConnect(ev.peer);
        } else {
            enet_peer_reset(ev.peer);  // defensive — shouldn't happen
        }
    }
    break;
}
```
The rule is purely positional: am I the first connection this host
has seen? No payload inspection needed. Clients never accept
spectators (any inbound CONNECT on a client is the opponent
acknowledging the client's outbound `enet_host_connect`).

**Fix 2 — `peerCapacity = 16` (was hardcoded 2)**
(`network_thread.cpp:79-94`):
```cpp
const std::size_t peerCapacity = 16;
```
Now accommodates the full spectator topology: 1 opponent + up to 15
spectators (`MAX_SPECTATORS = 15`). The previous Wine regression
(ENet host stopped receiving CONNECT events when `peerCapacity > 2`)
is being re-tested at full 16 capacity. If it resurfaces, the
fallback is to gate `peerCapacity` on `isHost_` (host=16, client=2)
since clients never accept spectators.

**Fix 3 — DISCONNECT handler delegates spectator disconnects**
(`network_thread.cpp:385-413`):
```cpp
case ENET_EVENT_TYPE_DISCONNECT: {
    if (ev.peer == peer_) {
        peer_ = nullptr;
        connected_.store(false, std::memory_order_release);
    } else if (spectatorMgr_) {
        spectatorMgr_->onSpectatorDisconnect(ev.peer);
    } else {
        // Unknown peer — already cleaned up by ENet.
    }
    break;
}
```
Mirrors the CONNECT rule: opponent disconnect clears `peer_` +
`connected_`; spectator disconnect delegates to the SpectatorManager;
unknown peer (stale socket, race during shutdown) is logged and
ignored — ENet has already cleaned up internally.

**Build validation:** MinGW-w64 i686 13-win32 cross-compile clean —
zero warnings, zero errors. `caster.exe` (5.7 MB) + `hook.dll`
(3.7 MB) + `caster.zip` (3.8 MB) all generated.

**Runtime validation matrix (pending — requires Wine+MBAACC.exe):**
1. Regression: host + join via localhost — must still work end-to-end
   (connect, handshake, launch, play). This is the critical check
   because Fix 2 (`peerCapacity = 16`) was the one that previously
   caused the Wine regression.
2. New: host + join + 3rd instance spectates via `--spec=127.0.0.1:PORT`
   — spectator should receive SpectateConfig + InitialGameState +
   BothInputs stream and replay the match.
3. New: relay spectate via `--spec=#room` — spectator connects via
   relay, host identifies as spectator (peer_ already set), spectator
   replays.
4. New: spectator disconnect mid-match — host's SpectatorManager
   removes the spectator via `onSpectatorDisconnect`, no crash, no
   impact on the ongoing match.
5. New: opponent disconnect mid-match with spectator connected —
   spectator should also be disconnected (or notified) since the
   BothInputs stream stops.

If runtime validation surfaces the Wine `peerCapacity > 2` regression
again, apply the host/client gate fallback described above.

### Layer 6 — Spectator client-side

**Status: ✅ implementado e wired (ativado automaticamente quando
o launcher envia `is_spectator = true` via IPC config).**

- [x] Create `src/dll/spec/spectate_client.hpp` and `.cpp`
  - [x] Receive BothInputs → push to game thread queue
  - [x] Receive RngState → push to game thread queue
  - [x] Receive MenuIndex → push to game thread queue
  - [x] Receive SpectateConfig → configure NetplayManager as spectator
  - [x] Receive InitialGameState → write chara/moon/color/stage to
        game memory + force FSM to AutoCharaSelect
- [x] Integrate into FSM:
  - [x] `AutoCharaSelect` state: auto-navigate chara-select if late-join
  - [x] `getInput()` returns 0 by default for SpectateNetplay mode
        (no explicit path in the dispatcher — falls through to default
        which returns 0; correct but worth verifying at runtime)
  - [x] No resend, no rollback (spectator has `is_netplay = false`,
        so the spin-lock gate at `frameStep` is skipped entirely —
        spectator just drains BothInputs and renders)
- [ ] Fast-forward / hard-sync controls (toggle with hotkey) —
      **deferred**, not blocking basic spectator replay
- [ ] Runtime validation of `frameStepRerun` for spectator —
      the rerun path was NOT explicitly adapted for spectator
      mode. Spectator shouldn't trigger rollback (just replay),
      but if `getLastChangedFrame` misbehaves when both players'
      inputs are written via `setBothInputs`, the spectator could
      enter the rollback path spuriously. Needs runtime check.

**Effort:** ~200 LOC real (was ~150 estimated). **Risk:** medium.

**Pendências runtime para Layers 5-6** (todas requerem Wine+MBAACC,
não reproduzíveis só com cross-compile):
- `SpectatorManager::step()` detecta timeout mas só faz
  `_pending.erase(peer)` — não chama `enet_peer_disconnect_later`.
  O comentário no código admite: "TBD — for now, we rely on the
  spectator's own client-side timeout." Pequeno gap, não bloqueia
  spectator básico.
- `frameStepRerun` não foi adaptado pra spectator — pode funcionar
  como está (spectator não tem inputs locais pra prever), mas
  precisa validação runtime.
- `getInput()` dispatcher não tem path `SpectateNetplay` explícito
  — retorna 0 por default, que é o correto pra spectator. Verificar
  se chega ao `writeGameInput` corretamente.

---

### Risks and mitigations (DLL-side)

| Risk | Mitigation |
|------|------------|
| Race condition in `NetplayManager` | Single global mutex (Decision 1) + `*Locked` convention (mandatory code review check) |
| Self-deadlock on `NetplayManager::_mutex` (public → public call) | `*Locked` suffix convention; no public function may call another public function |
| `RollbackManager::loadState` touches `NetplayManager` internals | `RollbackManager` acquires `_mutex` before writing `_state` / `_indexedFrame` (Decision 1, subtask 4.5) |
| Deadlock between game thread and network thread | Lock ordering: network thread always acquires `_mutex` only after pushing to inbox queues (not while holding them); game thread always drains inbox before acquiring `_mutex` |
| D3D9 not thread-safe | Network thread never touches D3D9 — only game thread renders |
| Game memory not thread-safe | Network thread never writes to `CC_*_ADDR` — only game thread does |
| SDL2 not thread-safe for joysticks | Network thread never touches SDL — only game thread polls joysticks |
| Debugging multi-threaded DLL | Logs already have thread_id; TSan if feasible (subtask 4.9); `assert` on thread affinity in debug builds as fallback |
| ENet packet ordering with queue | Queue is FIFO per type (`BlockingQueue<T>`); ENet reliable channels preserve order; unreliable channels don't need queue ordering (InputsContainer is keyed by frame, not arrival order) |
| `DLL_PROCESS_DETACH` during active network thread | `stop()` called in `netplay::shutdown()` BEFORE `deinitialize()`; jthread auto-joins on destruction; explicit assert in debug that thread is stopped before unhooking |
| `std::rand()` data race | Replace with `std::mt19937` member of `NetworkSimulator` (Decision 3) |
| Loss of 1653+ rollback validation coverage | Subtask 4.8 explicitly re-runs the `CASTER_SIM_LAG_MS=100` test; any SyncHash regression blocks merge |
| Phase B (speculative rollback) changes NetplayManager state shape | Global mutex (Decision 1) defers fine-grained design to when Phase B's shape is known — no lock redesign cost paid twice |

### Estimated effort

| Layer | LOC | Sessions | Depends on |
|---|---|---|---|
| 4 — Network thread foundation | ~350-450 | 3-4 | Layers 0-3 (launcher, done) |
| 5 — Spectator host-side | ~200 | 1 | Layer 4 |
| 6 — Spectator client-side | ~150 | 1 | Layer 4 + 5 |
| **Total** | **~700-800** | **5-6** | |

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
- 2026-07-14 — Layer 4 design decisions resolved: (1) global mutex + `*Locked` convention, (2) 5 separate `BlockingQueue<T>`, (3) `NetworkSimulator` as isolated class built now, (4) TSan feasibility investigation. Subtasks 4.1-4.9 defined. Ready to start implementation.
- 2026-07-14 — Subtask 4.1 complete: `src/dll/netplay/network_thread.{hpp,cpp}` created. Scaffolding only — jthread + ENetHost ownership + 5 inbox BlockingQueues + outbox queue. Loop logs connect/disconnect/receive events but doesn't route to inboxes yet. Added to CMakeLists.txt hook target. Syntax-checked with stub ENet header (full MinGW build pending).
- 2026-07-14 — Subtask 4.2 complete: `src/dll/netplay/network_simulator.{hpp,cpp}` created. Standalone class extracted from connector.cpp's inline simulator. `std::mt19937` member (replaces `std::rand()`), optional `CASTER_SIM_SEED` for reproducible runs, env vars read in `configure()` before jthread spawn. Added to CMakeLists.txt. `NetworkThread` now owns a `NetworkSimulator sim_` member, calls `configure()` in `start()` and `clear()` in `stop()`.
- 2026-07-14 — Subtasks 4.3 + 4.4 complete: `connector.cpp` rewritten as thin facade over `NetworkThread`. All ENet state (g_host, g_peer, g_connected, 5 inboxes, simulator) moved into NetworkThread. `poll()` is now a no-op. `connector.cpp` keeps the same public API for source compatibility with dll_main.cpp. NetworkThread::loop() implements the full receive/send/sim path — receives ENet events, decodes, routes to matching inbox BlockingQueue (with NetworkSimulator hooks for PlayerInputs), drains outbox via enet_peer_send.
- 2026-07-14 — Subtask 4.5 complete: `NetplayManager` now has `mutable std::mutex _mutex`. All public functions acquire the lock and dispatch to a `*Locked` private helper. Convention enforced: no public function may call another public function (would self-deadlock on the non-recursive mutex). RollbackManager friend access DEFERRED to follow-up commit — its `loadState`/`saveState` need `std::lock_guard` added before the 4.8 manual test.
- 2026-07-14 — Subtasks 4.6 + 4.7 complete: `dll_main.cpp` no longer calls `netplay::poll()` (removed from top of frameStep and from inside the spin-lock gate). `drainNetplayInbox()` unchanged — does non-blocking try_pop on the 5 BlockingQueues. Spin-lock stays (Phase B will remove it); network thread keeps running in the background while spin-lock blocks. `DLL_PROCESS_DETACH` ordering now explicitly documented: `netplay::shutdown()` (stop+join jthread, then destroy ENetHost) → `SDL_JoystickClose` → `deinitialize()` (unhooks). Layer 4 implementation complete pending 4.8 manual test (requires MinGW cross-compile).
- 2026-07-14 — Subtask 4.5 follow-up: `RollbackManager::saveState` and `loadState` now acquire `NetplayManager::_mutex` via `std::lock_guard` before touching `_state`/`_startWorldTime`/`_indexedFrame` (friend access). Closes the only remaining race in Layer 4 — the network thread's `setInputs`/`setRngState` no longer race with the game thread's rollback save/load.
- 2026-07-14 — **Subtask 4.8a (build) complete**: MinGW-w64 i686 16.1.0 cross-compile on Cachyos Linux. `caster.exe` (5.99 MB) + `hook.dll` (3.96 MB) built clean — **ZERO warnings, ZERO errors** across all 11 modified/new source files (network_thread.cpp, network_simulator.cpp, connector.cpp, manager.cpp, manager.hpp, rollback_manager.cpp, dll_main.cpp). Both binaries stripped and zipped to `release/caster.zip` (4.09 MB). Subtask 4.8b (manual runtime test: host+join + 1653 rollback regression) requires Windows/Wine + MBAACC.exe — pending.
- 2026-07-14 — **Subtask 4.8b (runtime) core regression PASSED**: user-validated host+join via localhost between two caster.exe instances — online netplay works end-to-end. Stress-test variants (CASTER_SIM_LAG_MS=100 for the 1653 rollback reproduction, jitter+loss, disconnect mid-match, force-kill) remain pending. The hardest regression target (1653+ rollbacks without desync) still needs validation under the simulator before declaring Layer 4 fully merged.
- 2026-07-14 — `scripts/build.sh` hardened against stale CMake cache. New pre-configure guard detects when `CMakeCache.txt` was created without the MinGW toolchain file (CMAKE_CXX_COMPILER doesn't contain "mingw"), purges `build/`, and reconfigures cleanly. Validated by simulating a stale cache on the remote — guard fires correctly and build completes. Closes the "compilei mas dá erro de windows.h" footgun.
- 2026-07-14 — **Subtask 4.9 (TSan) complete — fallback path taken.** ThreadSanitizer is not viable on any MinGW toolchain (Clang-MinGW i686/x86_64 reject `-fsanitize=thread`; GCC-MinGW lacks the runtime lib). Fallback implemented: `src/dll/netplay/thread_affinity.hpp` provides debug-build asserts for thread affinity (`check_network_thread_only`) and lock ordering (`check_not_holding_netman_mutex` + `SCOPED_NETMAN_MUTEX_HELD` macro). Asserts are NO-OPs in Release. Validated with both Release and Debug builds on the remote — clean compile in both modes. Together with the `*Locked` convention and SyncHash desync detection, this provides the safety net for Layer 4 in lieu of TSan.
- 2026-07-14 — **Commit `027d9ee` shipped**: "Layer 4: DLL network thread + Phase B speculative rollback + Phase C spectator (disabled)". Single commit bundled: Layer 4 (subtasks 4.1-4.9 + RollbackManager lock_guard follow-up), Phase B (B1-B4 speculative rollback — see `implementing-real-rollback.md`), Phase C (spectator classes + protocol + launcher wiring — see `spectator-plan.md`). Build clean (zero warnings, zero errors). 29 files changed, +3798 / -734 LOC. Core netplay regression PASSED on localhost. Spectator mode is fully wired through launcher/GUI/session/dll_main/connector/CMakeLists but DISABLED at the network layer via two guards in `network_thread.cpp` (CONNECT handler + peerCapacity=2) — see "Layer 5 disablers" above.
- 2026-07-15 — **Doc fact-check pass.** All claims in this document cross-checked against the actual source files (network_thread.{hpp,cpp}, spectator_manager.{hpp,cpp}, spectate_client.{hpp,cpp}, manager.{hpp,cpp}, rollback_manager.cpp, connector.cpp, dll_main.cpp, thread_affinity.hpp, network_simulator.{hpp,cpp}, session.{hpp,cpp}, netplay_config.hpp, messages.hpp, decoder.cpp, config_buffer.hpp, concurrency.hpp, CMakeLists.txt, build.sh). Corrections applied: (1) `Snapshot` → `SessionSnapshot`; (2) `maybe_update_snapshot()` → `publish_snapshot()`; (3) added `StartSpectate` + `StartRelaySpectate` to Command variant; (4) corrected `drainNetplayInbox` inbox count (5+3, not 5); (5) marked Layer 5 + Layer 6 as implemented-but-disabled with full disabler explanation; (6) marked RollbackManager lock_guard follow-up as done (was DEFERRED). Gaps identified that block next milestones: see "Layer 5 disablers" + "Pendências runtime para Layers 5-6" above.
- 2026-07-15 — **Layer 5 re-enabled at network layer (post-`3fd525e`).** Three fixes applied to `src/dll/netplay/network_thread.cpp`: (1) CONNECT handler now distinguishes opponent (first inbound on host, or any inbound on client) from spectator (subsequent inbound on host when `peer_` is set), delegating spectator CONNECTs to `spectatorMgr_->onSpectatorConnect(peer)`; (2) `peerCapacity` raised from 2 to 16 (full spectator topology: 1 opponent + 15 spectators), with documented host/client-gate fallback if the Wine regression resurfaces; (3) DISCONNECT handler now delegates spectator disconnects to `spectatorMgr_->onSpectatorDisconnect(peer)`, mirroring the CONNECT rule. Build clean (MinGW-w64 i686 13-win32, zero warnings, zero errors). `caster.exe` 5.7 MB + `hook.dll` 3.7 MB + `caster.zip` 3.8 MB generated. Runtime validation under Wine+MBAACC.exe is the only remaining gate — see "Runtime validation matrix" in the Layer 5 section above.
