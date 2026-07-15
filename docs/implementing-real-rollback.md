# Implementing Real Rollback

Status: **Phase 0 + Phase B (B1-B4) implementadas em commit `027d9ee` (2026-07-14). Phase 1-4 pendentes.** Última atualização 2026-07-15.

---

## TL;DR

ReCaster (like CCCaster before it) originally did not have
**speculative** rollback netplay — the architecture used by GGPO,
Skullgirls, Killer Instinct, and SF6 where the game thread never blocks.

What it had was **delay-based rollback with bounded prediction**: the
game thread advanced up to `config.rollback` frames ahead of the latest
remote input, predicting the missing frames via `lastInputBefore`. When
the real remote input arrived and diverged from that prediction, the
engine rewound via `loadState` and re-simulated. This is real rollback —
but the game thread could still block on the spin-lock whenever the
remote fell more than `config.rollback` frames behind, which on
connections above ~80-100ms RTT produced noticeable stutter.

**As of commit `027d9ee` (2026-07-14), Phase B (B1-B4) is implemented:**

- **B1 — Increased rollback window**: `isRemoteInputReady` now uses
  `MAX_ROLLBACK(15)` instead of `config.rollback(4)` (manager.cpp:786-792).
  The game thread can run up to 15 frames ahead of the latest remote
  input before the spin-lock stalls. Escape hatch: `CASTER_DETERMINISTIC=1`
  env var reverts to `config.rollback` for debugging.
- **B2 — Early rollback trigger at step 6.5**: after `sendPlayerInputs`
  (step 3b) but before `writeGameInput` (step 7), the engine checks
  `isInRollback() && rollbackTimer == minRollbackSpacing &&
  getLastChangedFrame() < getIndexedFrame()` and triggers `loadState`
  + early return (dll_main.cpp:1583-1628). This catches divergence
  one frame earlier than the previous post-`writeGameInput` trigger.
- **B3 — Rerun path verified**: `frameStepRerun` already skips
  `setInput` / `sendPlayerInputs` / spin-lock / `saveState` via an
  early `return` at dll_main.cpp:1183. No regressions found.
- **B4 — Stateful predictor (opt-in)**: `CASTER_PREDICTOR=stateful`
  env var makes the predictor read `CC_P{1,2}_NO_INPUT_FLAG_ADDR`
  during prediction (manager.cpp:509-570). When the flag is set
  (opponent in hitstun/blockstun/attack-lock), predict neutral
  input instead of repeating the last input. Default behavior
  (`CASTER_PREDICTOR=last` or unset) keeps the original
  `lastInputBefore` prediction.

**What is NOT done (Phase 1-4 of this plan):**

- The spin-lock at dll_main.cpp:1452-1535 is still there. The network
  thread delivers packets to inboxes in the background, so the
  spin-lock no longer calls `netplay::poll()` — but it still blocks
  the game thread when the remote falls more than `MAX_ROLLBACK(15)`
  frames behind. Full removal is Phase 1 below.
- Divergence detection still happens on the game thread
  (`getLastChangedFrame` check). Moving it to the network thread
  is Phase 2 below.
- Replay burst optimization (skip `saveState` during rerun) is
  Phase 3 below.
- Advanced prediction (state-aware) is Phase 4 below — but B4
  already ships a basic version via the NO_INPUT_FLAG check.

**Prerequisites:**
- ✅ Launcher threading migration (Layers 0-3 of `threading-migration.md`)
  — UI no longer blocks on handshake or game launch. Done.
- ✅ DLL-side threading migration (Layer 4 of `threading-migration.md`
  Part 2) — dedicated network jthread + lock-free queues. Done in
  commit `027d9ee`. This was the hard prerequisite for speculative
  rollback.
- ⬜ DXVK integration (`future-improvements.md` #12) — stable frametimes,
  `DXVK_FRAME_RATE=60` handling. **Still pending** — was originally
  listed as a prerequisite but Phase B shipped without it. Validate
  whether the rollback improvements are perceptible without DXVK's
  stable frametimes.

**Estimated effort (remaining):** ~600-800 LOC on top of Layer 4 + Phase B,
across 3-5 sessions. (Original estimate was 800-1000 LOC; Phase B consumed
~200 LOC of that budget.)
**Risk:** high — touches the most delicate part of the DLL (the netplay
FSM and state save/restore). Desyncs are easy to introduce and hard to
debug.

---

## Why CCCaster/ReCaster doesn't have speculative rollback

### What "speculative rollback" actually means

In a speculative rollback netcode (GGPO):

1. **The game never waits for the network.** Every frame, the local input
   is combined with a *predicted* remote input and the game advances
   immediately at 60 FPS.

2. **Prediction is cheap and usually right.** The simplest predictor
   ("the remote player keeps doing what they were doing") is correct
   >90% of the time in a fighting game, because inputs are sparse — most
   frames the opponent is holding a direction or doing nothing.

3. **Rollback is the exception, not the rule.** When the real remote
   input arrives and differs from the prediction, the engine:
   - Loads the saved state from the frame the prediction started going wrong
   - Re-simulates forward to the current frame using the correct inputs
   - The player sees a brief visual correction (a few frames of "flicker")

4. **The network runs on a separate thread.** Packets are sent/received
   asynchronously. The game thread reads from a lock-free queue and never
   stalls on `recv()`.

### What CCCaster/ReCaster actually does

CCCaster's architecture (ported faithfully into ReCaster — `manager.cpp`
header reads *"the NetplayState FSM is delicate enough that 'improvements'
here tend to introduce desyncs"*) is:

```
Frame N (netplay InGame, rollback enabled):
  1. clearLastChangedFrame()           // (1a in dll_main.cpp)
  2. updateFrame() from CC_WORLD_TIMER_ADDR
  3. Read local controller → setInput(localPlayer)
  4. sendPlayerInputs(localPlayer) to peer  // unreliable ENet
  5. Host: generate + send RngState
  6. drainNetplayInbox() → setInputs(remotePlayer)
  7. SPIN-LOCK GATE (dll_main.cpp:1315-1390):
       while (!isRemoteInputReady() || !isRngStateReady()) {
           netplay::poll();
           drainNetplayInbox();
           if (first_iter) Sleep(1); else Sleep(POLL_TIMEOUT_MS);
           resend inputs every 100ms;
           timeout after 10s → delayedStop("Timed out!");
       }
  8. Apply pending RngState (client only)
  9. Rollback timer countdown
 10. writeGameInput(both players)   // via getInput() — uses prediction
 11. saveState() every InGame frame
 12. Rollback trigger: if getLastChangedFrame() < getIndexedFrame()
        && rollbackTimer == minRollbackSpacing:
        loadState(target) → set fastFwdStopFrame → return
 13. (rerun path on next frames) frameStepRerun() replays with corrected
     inputs until fastFwdStopFrame is reached
 14. SyncHash exchange + desync detection (every 150 frames)
```

Step 7 is a **blocking spin-lock** on the game thread. The crucial nuance
is that it does **not** block every frame — `isRemoteInputReady()`
(`manager.cpp:657-712`) allows the game to advance up to
`config.rollback` frames ahead of the latest received remote input:

```cpp
const uint8_t maxFramesAhead = isInRollback() ? config.rollback : 0;
if ((remoteEndFrame - 1 + maxFramesAhead) < getFrame())
    return false;   // <-- only here does the spin-lock actually stall
return true;
```

So the actual behavior is:

- **Remote within `config.rollback` frames:** game advances with prediction
  (`getRawInput` returns `lastInputBefore` for the missing frames).
  No stall. Rollback fires if the prediction was wrong.
- **Remote more than `config.rollback` frames behind:** the spin-lock
  blocks the game thread, polling ENet with `Sleep(1)` / `Sleep(3)`
  until either the remote catches up or the 10s timeout fires.

For typical `config.rollback = 4-7`:

- **At 50ms RTT (~1.5 one-way frames):** spin-lock almost never fires.
  Game runs at 60 FPS. Rollback corrects the occasional misprediction.
- **At 100ms RTT (~3 one-way frames):** spin-lock fires intermittently
  during jitter spikes. Mostly smooth with occasional hitch.
- **At 150ms RTT (~4.5 one-way frames):** spin-lock fires regularly when
  jitter pushes one-way delay past `config.rollback`. Noticeable stutter.
- **At 200ms RTT (~6 one-way frames):** spin-lock fires on most frames
  unless `config.rollback` is raised to 8+, at which point replay bursts
  get expensive. Unplayable feel.

### What "rollback" means in the current codebase

The `RollbackManager` (`rollback_manager.cpp`) is a real, working rollback
engine:

- `saveState()` is called every InGame frame and stores a full snapshot
  (game memory via `MemDumpList` + `NetplayManager` FSM state + FP env).
- `loadState(target)` walks `_statesList` in reverse, finds the newest
  state `<= target`, restores game memory + FSM, and applies the
  `RepInputContainer` fixup (decrements the game's internal replay
  frame counts so saved replays stay consistent).
- Rollback is triggered by **prediction divergence**, not by late packets:
  `getLastChangedFrame()` returns the earliest frame where a received
  remote input disagreed with what `getRawInput` would have predicted.
  The trigger condition (`dll_main.cpp:1494-1496`) is
  `isInRollback() && rollbackTimer == minRollbackSpacing &&
   getLastChangedFrame().value < getIndexedFrame().value`.
- The replay path (`frameStepRerun` at `dll_main.cpp:1029-1043`) replays
  frames with corrected inputs, skipping saveState/trigger during rerun.
- `SyncHash` (xxHash128 of game state, exchanged every 150 frames) detects
  desyncs.

The `port-status.md` validation log records 1653+ rollbacks during a
single test match with no desync — so rollback is exercised, not dormant.

### Why it was built this way

CCCaster was written in 2014 for a game (MBAACC) that:
- Has no deterministic replay system — state save/restore is done by
  raw memory dumping (~1.18 MB per frame; `CC_GRAPHICS_ARRAY_SIZE` alone
  is 384 KB and `CC_EFFECTS_ARRAY` is 828 KB)
- Has no frame-perfect state isolation — some game state leaks between
  frames in ways that make aggressive rollback risky (the `MemDumpPtr`
  dangling-parent bug fixed in commit `c78f938` is an example)
- Was primarily played on LAN or with <50ms connections

The delay-based approach is simpler to implement correctly: by bounding
how far the local side can run ahead, you bound how many frames you ever
need to replay, which bounds the risk of hitting an uncapturable state
delta. The cost is that the game stalls on any connection where jitter
pushes the one-way delay past `config.rollback`.

ReCaster inherited this architecture because the FSM is "delicate enough
that improvements tend to introduce desyncs" (per the `manager.cpp:7-8`
comment). This document is the plan to finally fix that — carefully.

---

## Architecture: Speculative Rollback

### Target state

```
┌─────────────────────────────────────────────────────┐
│  Game thread (MBAACC main loop, hooked)             │
│                                                     │
│  Frame N:                                           │
│    1. Read local input                              │
│    2. Read remote input from SPSC queue (non-block) │
│       - If queue empty: use PREDICTED remote input  │
│       - If queue has input: use REAL input          │
│    3. Check rollback flag (atomic)                  │
│       - If set: loadState(rollback_frame)           │
│                  replay frames rollback_frame..N    │
│    4. Save state (for potential future rollback)    │
│    5. Write inputs to game memory                   │
│    6. Game advances 1 frame                         │
│                                                     │
│  Never blocks. Never calls Sleep. Always 60 FPS.    │
└─────────────────────────────────────────────────────┘
          ▲                          ▲
          │ inputs (local)           │ rollback signal
          │                          │ (atomic flag + frame#)
┌─────────┴──────────┐    ┌─────────┴──────────────┐
│  Network thread     │    │  (same thread)         │
│  (jthread)          │    │                        │
│                    │    │                        │
│  Loop:              │    │                        │
│    enet_host_service│    │                        │
│    if packet recv:  │    │                        │
│      push to queue  │    │                        │
│      compare with   │    │                        │
│        prediction   │    │                        │
│      if mismatch:   │    │                        │
│        set rollback │    │                        │
│        flag + frame │    │                        │
│    send local input │    │                        │
│      (from queue)   │    │                        │
└─────────────────────┘    └────────────────────────┘
```

### Key components

#### 1. Lock-free SPSC input queue

Single-Producer Single-Consumer ring buffer for remote inputs. The
network thread is the sole producer; the game thread is the sole consumer.

```
struct RemoteInputEntry {
    uint32_t frame;       // which game frame this input is for
    uint16_t input;       // the actual input bits
};

class RemoteInputQueue {
    // Lock-free SPSC ring buffer, capacity = rollback_window + 8
    // (e.g. 16 entries for an 8-frame rollback window)
    //
    // Producer (net thread): push(entry) — never blocks
    // Consumer (game thread): pop() → optional<entry> — never blocks
    //                         peek_latest_frame() → uint32_t
};
```

No mutexes, no allocations. Power-of-2 capacity for `& (cap-1)` masking.

This depends on Layer 4 of `threading-migration.md` Part 2 — the
dedicated network jthread that owns the `ENetHost*`.

#### 2. Input prediction

The current code already implements this via `getRawInput` →
`InputsContainer::get` → `lastInputBefore`. The migration does not
change the predictor itself; it changes **when prediction is allowed to
happen** (always, instead of "only when remote is within
`config.rollback` frames").

The predictor: **repeat the last known remote input**.

```
uint16_t predictRemoteInput() {
    auto last = remote_queue.peek_latest();
    if (last) return last->input;
    return 0;  // neutral
}
```

This is correct because:
- In MBAACC, inputs are held across multiple frames (holding a direction)
- If the opponent does nothing new, the prediction is perfect
- If they do something new, we'll be wrong for 1-RTT/2 frames, then
  rollback corrects it

**Advanced prediction (later phase):** A small state machine that knows
"if the opponent was in hitstun, they're probably still in hitstun" etc.
Not needed for v1 — `lastInputBefore` already achieves >90% accuracy in
neutral play, as validated by the 1653-rollback test match.

#### 3. Rollback trigger

The current code already implements this via `getLastChangedFrame()` +
the `dll_main.cpp:1494-1496` trigger condition. The migration moves the
divergence detection from the game thread (during `drainNetplayInbox`)
to the network thread (on packet receive), and signals via atomics
instead of a mutable field on `NetplayManager`.

```
// Net thread, after receiving real input for frame F:
uint16_t predicted = prediction_for_frame(F);
if (real_input != predicted) {
    rollback_pending.store(true, memory_order_release);
    rollback_frame.store(F, memory_order_release);
    // Store the correct input so game thread can replay
    rollback_inputs.push({F, real_input});
}
```

The game thread checks this flag at the **start** of each frame, before
advancing:

```
// Game thread, start of frame N:
if (rollback_pending.load(memory_order_acquire)) {
    uint32_t rb_frame = rollback_frame.load(memory_order_acquire);
    // Load state from rb_frame
    g_rollMan.loadState(rb_frame);
    // Replay inputs from rb_frame to N-1 with correct remote inputs
    for (uint32_t f = rb_frame; f < current_frame; ++f) {
        uint16_t remote = getRealInputForFrame(f);
        uint16_t local = getLocalInputForFrame(f);
        writeGameInput(1, local);
        writeGameInput(2, remote);
        gameAdvanceOneFrame();  // via the existing callback
    }
    rollback_pending.store(false, memory_order_release);
}
```

This is functionally identical to the existing rerun path
(`frameStepRerun` at `dll_main.cpp:1029-1043`). The migration does not
rewrite the rerun; it removes the spin-lock that prevents rerun from
ever being needed at high latency.

#### 4. State save/load (existing, with caveats)

`RollbackManager::saveState` / `loadState` already exist and work. The
change is that:
- **saveState runs every frame** (already does — `dll_main.cpp:1484-1486`)
  — needed for potential rollback
- **loadState runs only when rollback is triggered** (currently happens
  whenever `getLastChangedFrame` < `getIndexedFrame`; with speculative
  rollback this will fire more often because prediction is allowed to
  run further ahead)

The 1.18 MB state save is the main CPU cost. Optimization options (separate
from this plan, but combinable):

| Option | Effort | Speedup |
|--------|--------|---------|
| Dirty-page tracking via `VirtualProtect(PAGE_GUARD)` | ~300 LOC | ~24× (50 KB vs 1.18 MB) |
| Flatten 1000 effect MemDump roots into 1 | ~100 LOC | ~2× |
| Compress state with LZ4 before storing | ~150 LOC | ~3-4× (but adds decompress latency) |

For v1 of speculative rollback, keep the existing saveState. Optimize
later if profiling shows it's the bottleneck (it probably won't be — the
big win is eliminating the stall, not making saveState faster).

#### 5. Input history buffer

To replay frames during rollback, we need to know what the **local** input
was for each past frame. The current code already stores this in the
`InputsContainer` indexed by `(transition_index, frame)` — the local
player's inputs are kept in `_inputs[_localPlayer - 1]` and read back via
`getRawInput(localPlayer, frame)` during rerun. No new data structure
needed; just make sure the container's capacity covers the new
(uncapped-by-spinlock) rollback window.

Remote inputs for past frames come from the `RemoteInputQueue` history
(the network thread keeps a ring buffer of all received inputs, mirroring
the `InputsContainer` on the remote side).

---

## Implementation phases

### Phase 0: Prerequisites (must be done first)

**Status: ✅ Completa (commit `027d9ee`, 2026-07-14).**

- [x] DLL-side threading migration Layer 4 complete
      (`threading-migration.md` Part 2)
  - [x] Dedicated network jthread owning the `ENetHost*`
        (`src/dll/netplay/network_thread.{hpp,cpp}`)
  - [x] `connector.cpp` refactored: `poll()` is now a no-op (kept for
        source compat), `send*` enqueues to network thread outbox
  - [x] `BlockingQueue<T>` from `src/common/concurrency.hpp` reused for
        Network → Game message queue (5 main + 3 spectator inboxes)
  - [x] Mutex added to `NetplayManager` for thread-safe access
        (`_mutex` + `*Locked` convention + `NETMAN_LOCK_GUARD` macro)
  - [x] Build + manual test: netplay host + join works identically to
        before (validated on localhost, end-to-end)
- [x] Launcher threading Layers 0-3 already done (UI non-blocking) —
      no further work needed for this plan
- [ ] DXVK integration complete (`future-improvements.md` #12)
  - Stable frametimes (no D3D9 driver stutter)
  - `DXVK_FRAME_RATE=60` active (our frame limiter disabled when DXVK on)
  - **Status: ainda pendente.** Phase B foi implementada sem DXVK;
    avaliar se os ganhos de rollback são perceptíveis sem frametimes
    estáveis antes de tratar como blocker para Phase 1.

### Phase B: Speculative rollback foundation (IMPLEMENTED in `027d9ee`)

**Status: ✅ Completa (commit `027d9ee`, 2026-07-14).**

Esta fase foi adicionada durante a implementação do Layer 4 e não
estava no plano original. Ela representa os ganhos "fáceis" de
speculative rollback que não requerem remoção do spin-lock:

- [x] **B1 — Increased rollback window** (`manager.cpp:786-792`):
      `isRemoteInputReady` agora usa `MAX_ROLLBACK(15)` (de
      `game_addresses.hpp:24`) em vez de `config.rollback(4)` quando
      em rollback. Escape hatch: `CASTER_DETERMINISTIC=1` env var
      reverte para `config.rollback` para debugging.
- [x] **B2 — Early rollback trigger at step 6.5** (`dll_main.cpp:1583-1628`):
      após `sendPlayerInputs` (step 3b) mas antes de `writeGameInput`
      (step 7), o engine checa `isInRollback() && rollbackTimer ==
      minRollbackSpacing && getLastChangedFrame() < getIndexedFrame()`
      e dispara `loadState` + early return. Captura divergência um
      frame mais cedo que o trigger anterior (que era pós-`writeGameInput`).
- [x] **B3 — Rerun path verified** (`dll_main.cpp:1170-1184`):
      `frameStepRerun` já pula `setInput` / `sendPlayerInputs` /
      spin-lock / `saveState` via early `return` na linha 1183.
      Nenhuma regressão encontrada.
- [x] **B4 — Stateful predictor (opt-in)** (`manager.cpp:509-570`):
      `CASTER_PREDICTOR=stateful` env var faz o predictor ler
      `CC_P{1,2}_NO_INPUT_FLAG_ADDR` (game_addresses.hpp:196, 221)
      durante predição. Quando o flag está setado (oponente em
      hitstun/blockstun/attack-lock), prediz input neutro em vez de
      repetir o último input. Default (`CASTER_PREDICTOR=last` ou
      unset) mantém o `lastInputBefore` original.

**Validação:** build clean (zero warnings, zero errors) + host+join
via localhost testado pelo usuário em 2026-07-14. Os stress tests
com `CASTER_SIM_LAG_MS=100` (regressão do rollback 1653+) ainda
estão pendentes — ver subtask 4.8 do `threading-migration.md`.

### Phase 1: Non-blocking input reads (low risk)

**Status: ⬜ Pendente.**

**Goal:** Replace the spin-lock gate at `dll_main.cpp:1452-1535` with a
non-blocking queue read. Prediction stays as the existing
`lastInputBefore` (or the B4 stateful predictor if `CASTER_PREDICTOR=stateful`
is set — not "predict neutral" by default, that would be a regression;
the current predictor is already correct).

- [ ] Implement `RemoteInputQueue` (SPSC ring buffer) on top of the
      Layer 4 message queue
- [ ] Network thread pushes received inputs to queue instead of via
      `drainNetplayInbox` → `setInputs` on the game thread
- [ ] Game thread reads from queue; if empty, falls back to
      `getRawInput(remotePlayer)` which already returns `lastInputBefore`
      (the previous frame's remote input) as prediction
- [ ] Remove the spin-lock at `dll_main.cpp:1452-1535` (line numbers
      shifted from the original 1315-1390 due to Phase B additions).
      Keep the disconnect check (`!netplay::connected()` → `delayedStop`)
      and the 10s overall timeout, but move them to the network thread's
      snapshot.
- [ ] **Test:** game should run at 60 FPS even with 200ms artificial
      latency. Remote character will appear to "do nothing" until inputs
      arrive, then jump to the correct position via rollback.

**Risk:** Low. The rollback path already exists and is exercised. We're
just removing the gate that prevents prediction from being used beyond
`config.rollback` frames.

### Phase 2: Divergence detection on the network thread (medium risk)

**Status: ⬜ Pendente.**

**Goal:** Move divergence detection off the game thread so rollback can
trigger as soon as the real input arrives, without waiting for the next
game-thread frame.

- [ ] Network thread: when real input arrives, compare with what was
      predicted for that frame (read the prediction from the
      `RemoteInputQueue`'s history). If mismatch, set the atomic
      `rollback_pending` flag + `rollback_frame`.
- [ ] Game thread: check `rollback_pending` at the start of each frame
      (step 3 in the target state diagram). If set, run the existing
      `loadState` + `frameStepRerun` path, then clear the flag.
- [ ] Remove the in-game-thread `getLastChangedFrame` check (currently
      at `dll_main.cpp:1583-1628` after Phase B's step 6.5 trigger —
      was 1494-1496 in the original layout). Replaced by the atomic
      flag set by the network thread.
- [ ] **Test:** with 100ms latency, opponent should appear to move
      smoothly. Occasional visual corrections (flicker) when they do
      something new — this is correct rollback behavior.

**Risk:** Medium. The atomic signalling must be airtight. If the network
thread sets the flag after the game thread has already passed the check
point but before `saveState`, the rollback will target a frame whose
state hasn't been saved yet. Mitigation: the existing `loadState`
RELEASE-fallback (force-load the front state if none `<=` target) covers
this case — verify it still works under the new trigger timing.

### Phase 3: Replay optimization (low risk)

**Goal:** Make the replay burst faster. The existing `frameStepRerun`
already skips `setInput` / `sendPlayerInputs` / spin-lock / rollback
save/trigger (see `dll_main.cpp:1023-1024` comment). Remaining wins:

- [ ] During replay, skip `saveState` (we're re-simulating, not advancing
      — `saveState` would just fill the pool with states we'll never
      roll back to). The rerun path already skips this via the early
      `return` at `dll_main.cpp:1042`; verify nothing regressed.
- [ ] During replay, skip the frame limiter (replay as fast as possible).
      Currently `CC_SKIP_FRAMES_ADDR = 1` is set during rerun
      (`dll_main.cpp:1517`), which already bypasses the limiter. Confirm.
- [ ] Profile: if replay of 8 frames takes >4ms, investigate
      `saveState`/`loadState` optimization (dirty-page tracking)
- [ ] **Test:** rollback of 8 frames should complete in <4ms (imperceptible)

### Phase 4: Advanced prediction (optional, low priority)

**Status: ⚠️ Parcialmente implementada (B4 é uma versão básica desta fase).**

**Goal:** Improve prediction accuracy to reduce rollback frequency.

- [x] Track opponent state via `CC_P{1,2}_NO_INPUT_FLAG_ADDR` (B4 —
      `manager.cpp:509-570`, opt-in via `CASTER_PREDICTOR=stateful`).
      Quando o flag está setado (oponente em hitstun/blockstun/attack-lock),
      prediz input neutro em vez de repetir o último input.
- [ ] Expandir para estados mais finos: distinguish "in hitstun" vs
      "in blockstun" vs "attacking" vs "neutral" via leitura de mais
      endereços de game memory (similar a `CC_INTRO_STATE_ADDR` etc).
- [ ] Predict based on state: if in hitstun, predict no input (they're
      stuck); if attacking, predict continuation of the attack
- [ ] **Measure:** rollback frequency before/after. Target: <5% of frames
      trigger rollback at 100ms RTT. (B4 ainda não tem essa medição —
      adicionar telemetria antes/depois de habilitar `CASTER_PREDICTOR=stateful`
      em produção.)

**Risk:** Low — prediction logic is pure (doesn't touch game state, just
reads it). Wrong predictions don't cause desyncs, just more rollbacks.

---

## Risk analysis

### Desyncs (highest risk)

**Cause:** If saveState/loadState doesn't capture ALL state that affects
gameplay, replaying frames with different inputs will diverge from what
would have happened if the inputs were known at the time.

**Mitigation:**
- The existing `SyncHash` system (xxHash128 of game state, exchanged
  every 150 frames — see `dll_main.cpp:1554-1571`) already detects
  desyncs. Keep it active.
- Add a **deterministic mode** (environment variable `CASTER_DETERMINISTIC=1`)
  that forces lockstep (disables speculation, restores the spin-lock) for
  debugging. If a desync is reported, ask the user to reproduce in
  deterministic mode.
- Log every rollback: frame number, predicted vs real input, replay depth.
  The existing `caster::dll::netplay_debug::log_event("rollback-trigger", ...)`
  call at `dll_main.cpp:1505-1507` is the seed of this log; extend it.

**Recovery:** If desync detected mid-match, the only safe recovery is to
pause and resync (save both states, advance to a known frame). This is
disruptive but better than a silent desync that ruins the match.

### Thread safety

**Cause:** The network thread and game thread both access the input
queues. If the queues aren't truly lock-free, or if there's shared state
we missed, races will cause corrupt inputs.

**Mitigation:**
- SPSC queues are race-free by construction (one producer, one consumer,
  no shared write locations).
- The only shared mutable state is `rollback_pending` / `rollback_frame`
  (atomics) and the input queues (SPSC). Everything else is thread-local.
- Use `std::atomic` with `memory_order_acquire/release` for the rollback
  signal. Never `volatile` (matches `threading-migration.md` guiding
  principle #8).
- The `NetplayManager` mutex from Layer 4 protects the FSM fields that
  both threads touch (`_inputs`, `_state`, `_indexedFrame`).

### Replay correctness

**Cause:** During replay, the game must advance frames exactly as it
would have if the correct inputs were known. If the replay path differs
from the normal path (e.g., skips a hook, doesn't update some counter),
the state will diverge.

**Mitigation:**
- The replay loop (`frameStepRerun`) calls the same `writeGameInput` +
  game-advance callback as normal play. The only difference is that
  inputs come from the history buffer instead of live reads, and
  `saveState` / `sendPlayerInputs` / spin-lock / rollback-trigger are
  skipped (see `dll_main.cpp:1023-1024`).
- The existing rollback code already does this correctly — it's been
  validated with 1653+ rollbacks in a single match without desync
  (per `port-status.md`).

### MBAACC engine quirks

**Cause:** MBAACC was not designed for rollback. Some game state may not
be capturable by the MemDump system (e.g., internal engine timers, audio
state, RNG that advances on wall-clock instead of frame count).

**Mitigation:**
- The RNG is already synchronized via `RngState` messages (host generates,
  client applies — see `dll_main.cpp:1404-1418`). This is the most
  critical piece.
- Audio state doesn't affect gameplay — if sounds are slightly off during
  replay, that's acceptable. The SFX history from CCCaster was already
  removed in ReCaster v1 (see `rollback_manager.hpp` header).
- The `RepInputContainer` fixup in `loadState` (`rollback_manager.cpp:213-254`)
  keeps the game's internal replay struct consistent with the netplay
  state. This was ported 1:1 from CCCaster and is critical for replay
  save/load correctness.
- If an uncapturable state is found, the SyncHash will catch it (desync
  detected, match pauses for resync).

---

## Testing strategy

### Unit tests

- `RemoteInputQueue`: push/pop ordering, overflow behavior, empty-queue
  peek
- `LocalInputHistory` (or the existing `InputsContainer` used as such):
  ring buffer wraparound, `get(frame)` for past frames

### Integration tests (local, two instances)

1. **Latency test:** Run two instances on localhost with 0ms, 50ms, 100ms,
   200ms artificial latency (via Windows Firewall delay or a proxy).
   Verify: both instances stay in sync (SyncHash matches), game runs at
   60 FPS on both.

2. **Rollback frequency test:** At 100ms latency, log rollback count per
   60-frame second (the `netplay_debug::log_event("rollback-trigger")`
   call already produces this data). Target: <5 rollbacks/second during
   neutral play, <15 during active combat.

3. **Desync detection:** Intentionally corrupt one side's state (flip a
   bit in memory). Verify: SyncHash detects it within 150 frames and
   pauses for resync.

4. **Deterministic mode regression:** With `CASTER_DETERMINISTIC=1`,
   verify the game behaves identically to pre-migration (spin-lock
   restored, no speculation). This is the escape hatch if a desync
   shows up in the wild.

### Real-world test

- Play 10 matches against a remote opponent at varying latencies
- Verify: no crashes, no permanent desyncs, game feels responsive at
  100-150ms RTT
- Compare to pre-change: same matches should feel significantly smoother

---

## Rollback window sizing

The rollback window (max frames we'll replay) must be ≥ the one-way
delay in frames:

```
rollback_window = ceil(rtt_ms / 2 / 16.67) + 2  // +2 for jitter
```

| RTT | One-way frames | Rollback window |
|-----|----------------|-----------------|
| 50ms | 1.5 | 4 |
| 100ms | 3 | 5 |
| 150ms | 4.5 | 7 |
| 200ms | 6 | 8 |
| 300ms | 9 | 11 |

The existing `NUM_ROLLBACK_STATES` is **60 in release (NDEBUG) builds
and 256 in debug builds** (`addresses.hpp:27-31`). 60 is more than
enough for the practical window — the practical limit is replay time:
replaying 11 frames at ~1ms each = 11ms, which is 66% of a frame budget.
Acceptable but noticeable.

`MAX_ROLLBACK = 15` (`addresses.hpp:24`) is the hard cap on
`config.rollback` — the user-configurable delay. With speculative
rollback, this becomes the max replay depth per rollback event.

**Cap:** If the one-way delay exceeds 12 frames (~200ms RTT), fall back
to lockstep for that session (set `rollback_window = 0`, which reverts to
the old blocking behavior via the `CASTER_DETERMINISTIC` path). Better
to stall than to replay 15+ frames every time.

---

## Non-goals

- **Spectator mode** — separate feature, depends on this but not part of
  it. See `spectator-plan.md`. (Layer 5-6 of `threading-migration.md`
  Part 2 is the spectator plan; speculative rollback unblocks it by
  eliminating the spin-lock that currently prevents accepting spectator
  connections mid-frame.)
- **State compression** — orthogonal optimization. Can be added after
  speculative rollback is working.
- **Replay save/load** — separate feature. The `RepInputContainer` fixup
  in `loadState` already keeps the game's replay struct consistent, so
  this is forward-compatible.
- **2v2 online** — separate feature, needs 4-player rollback (significantly
  more complex).

---

## Success criteria

1. **60 FPS at 100ms RTT** — game thread never stalls; FPS counter stays
   at 60 ±1 during gameplay.
2. **<5 rollbacks/second** during neutral play at 100ms RTT.
3. **Zero desyncs** in 10 matches against a remote opponent.
4. **Subjective feel** — players report the game feels "like offline" or
   "like good rollback" (comparable to GGPO-based games).
5. **Deterministic mode works** — `CASTER_DETERMINISTIC=1` restores
   pre-migration behavior with no regressions, as the escape hatch for
   debugging.
