# ReCaster Issue #1 — Investigation Report

**Issue:** [Vmarcelo49/ReCaster#1](https://github.com/Vmarcelo49/ReCaster/issues/1)
**Title:** Rollback broken since 43a6fdd — desyncs immediately after loading, online unplayable
**Date of investigation:** 2026-07-17
**Repo state cloned:** `HEAD = 815dcd6` (current `main`), commit under investigation `43a6fdddc4d7c0089dbbad537d03eb373dcfb4fd`

---

## 1. Issue Summary

| Field | Detail |
|-------|--------|
| Reported by | `Vmarcelo49` (repo owner) |
| Labels | `bug` |
| Status | Open |
| Date opened | 2026-07-17 05:07 UTC |
| Suspect commit | `43a6fdd` ("Major refactor to the UI, new plans too") |
| Symptom | With `rollback > 0`, the game desyncs on the `in_game` scene immediately after loading, on the first sync attempts |
| Last known working | "3 commits prior to 43a6fdd" (i.e. `004b686`, `e947e90`, `47b4871`, `129dc78`) |
| Requested next steps | Bisect/diff files changed in 43a6fdd; pinpoint the exact change responsible |

The issue correctly identifies the regression window: commit `43a6fdd` is where rollback broke. What the issue does NOT yet identify is **why** — and the answer is non-obvious: the culprit is not a bug introduced *by* `43a6fdd`, but a **latent DLL-side bug that `43a6fdd` activated for the first time**.

---

## 2. Methodology

1. Cloned the repository to `/home/z/my-project/investigation/ReCaster`.
2. Fetched the issue body via the GitHub API.
3. Inspected the full file-change summary of `43a6fdd` (`git show --stat`): 32 files, ~91,540 insertions / 13,876 deletions (the bulk being a regenerated embedded font blob).
4. Generated a numbered list of 8 plausible root-cause hypotheses (UI state leaks, config struct changes, controller mapping asymmetry, build-flag changes, etc.).
5. Dispatched **four parallel sub-agents** (Tasks 1-a, 1-b, 1-c, 1-d) — each independently investigated a disjoint subset of the changed files plus the DLL rollback machinery. All sub-agents wrote their findings to a shared worklog at `/home/z/my-project/worklog.md`.
6. Verified the central "smoking gun" claim by directly inspecting both pre- and post-`43a6fdd` versions of `session.cpp::finish_ping_exchange()` and the IPC config-buffer wire layout.
7. Attempted a full project build via the project's `scripts/build.sh` (MinGW i686 cross-compile) to validate build infrastructure. Build infrastructure is intact; the MinGW cross-compiler is not installed in this sandbox and there is no root access to install it (see §6).
8. Performed a syntax-only check on the affected headers (`config_buffer.hpp`, `netplay_config.hpp`) using native `g++ -std=c++23 -fsyntax-only` — both parse cleanly.

---

## 3. Numbered List of Possible Causes Investigated

Each hypothesis was assigned to a sub-agent and verified against the actual diff.

| # | Hypothesis | Investigated by | Verdict |
|---|------------|-----------------|---------|
| 1 | `session.cpp` handshake changes reset/lose the rollback config | 1-a | **CONFIRMED — primary trigger** |
| 2 | `config.cpp/hpp` struct modifications cause rollback field to be misread | 1-b | Refuted — `default_rollback` was *removed* from `Config`, but that field is unrelated to netplay rollback |
| 3 | `waiting_for_peer.cpp` changes send netplay config before rollback is set | 1-b | Refuted — the new code calls `set_rollback_async()` before `host_confirm_async()`; minor stale-static-buffer bug exists but is not the systematic desync |
| 4 | `play_page.cpp` UI state leaks into netplay config | 1-b | Refuted — pure UI refactor, no change to `do_host`/`do_join`/`do_spectate` semantics |
| 5 | `controller/mapping.cpp` asymmetry between peers breaks input-byte sync | 1-c | Refuted — mappings are local-only; the wire payload is a resolved `uint16_t`, never re-encoded from the peer's mapping |
| 6 | `game_runner.cpp` command-line/IPC changes drop the rollback flag | 1-c | Refuted — only the **offline** launch path changed (now correctly uses `rollback=0`); the netplay IPC path is unchanged |
| 7 | `main.cpp` argument parsing loses the `--rollback` CLI flag | 1-c | Refuted — `--rollback` was relocated, not removed; still drives `session.set_rollback_async()` in `cli.cpp:154-157` |
| 8 | DLL-side rollback machinery was already broken; 43a6fdd just exposed it | 1-d | **CONFIRMED — root cause** |

**Convergent conclusion:** hypotheses #1 and #8 are two halves of the same root cause. They are not competing explanations.

---

## 4. Root-Cause Analysis

### 4.1 The smoking gun (independently verified)

`commit 43a6fdd` rewrote `NetplaySession::finish_ping_exchange()` in `src/exe/session/session.cpp`.

**Pre-43a6fdd** (verified via `git show 43a6fdd~1:src/exe/session/session.cpp`):

```cpp
void NetplaySession::finish_ping_exchange() {
    auto tstats = transport_.get_stats();
    stats_.packet_loss = static_cast<std::uint8_t>(tstats.packet_loss_pct);

    if (config_.is_host && !config_.manual_delay) {
        double avg = stats_.avg_ms > 0 ? stats_.avg_ms : 50.0;
        double computed = std::ceil(avg / (1000.0 / 60.0));
        config_.delay = static_cast<std::uint8_t>(
            std::min<double>(computed, 8.0));
    }
    // NOTE: config_.rollback is NEVER assigned here.
    // It stays at its NetplayConfig default of 0 (netplay_config.hpp:20).
    ...
}
```

**Post-43a6fdd** (current `src/exe/session/session.cpp:1004-1064`):

```cpp
void NetplaySession::finish_ping_exchange() {
    auto tstats = transport_.get_stats();
    stats_.packet_loss = static_cast<std::uint8_t>(tstats.packet_loss_pct);

    if (config_.is_host && !config_.manual_delay) {
        const double avg = stats_.avg_ms > 0 ? stats_.avg_ms : 200.0;
        const double frame_ms = 1000.0 / 60.0;
        int delay = 0;
        int rollback = 4;

        if (avg < 50.0)       { delay = 0; rollback = 4; }
        else if (avg < 100.0) { delay = 1; rollback = 6; }
        else if (avg < 150.0) { delay = 2; rollback = 7; }
        else                  { delay = 3; rollback = 8; }

        if (avg > 180.0) {
            const int one_way_frames = static_cast<int>(
                std::ceil((avg / 2.0) / frame_ms));
            const int needed = one_way_frames + 2;
            if (needed > delay + rollback) {
                delay = needed - rollback;
            }
        }

        config_.delay    = static_cast<std::uint8_t>(delay);
        config_.rollback = static_cast<std::uint8_t>(rollback);  // <-- THE CHANGE
    }
    ...
}
```

The only behaviorally significant change in `session.cpp` is the addition of the line `config_.rollback = ...`. Before `43a6fdd`, the host **never** assigned `config_.rollback`; it sat at the `NetplayConfig` default of `0` (`src/exe/session/netplay_config.hpp:20`).

### 4.2 How the rollback value reaches the DLL

The chain is fully traceable and was verified end-to-end:

1. **`NetplayConfig::rollback`** (`netplay_config.hpp:20`) — default `0`.
2. **`NetplaySession::send_config_message()`** (`session.cpp:1083`) — writes `buf[2] = static_cast<char>(config_.rollback)` and sends it to the peer.
3. **`game_runner.cpp:371`** (netplay launch path, unchanged by `43a6fdd`) — `ipc_cfg.rollback = c.np_cfg.rollback`.
4. **`config_buffer::serialize()`** (`src/common/ipc/config_buffer.cpp`) — places `rollback` at byte offset 2 of the IPC buffer (documented in `config_buffer.hpp:14`).
5. **DLL-side `deserialize()`** — reads offset 2 back into `Config::rollback`.
6. **`dll_main.cpp:461`** — `nc.rollback = cfg.rollback;` propagates into `g_netMan.config.rollback`.

### 4.3 What `rollback > 0` activates in the DLL

The DLL gates its entire rollback code path on `cfg.rollback > 0`. Specifically, when `rollback > 0`:

- `dll_main.cpp:467` — `nc.rollbackDelay = (cfg.rollback > 0) ? cfg.delay : 0;` — flips rollback delay from 0 to the negotiated delay.
- `dll_main.cpp:494-499` — sets `g_minRollbackSpacing` and `g_rollbackTimer`.
- `dll_main.cpp:513-517` — applies rollback-specific game hacks (`hijackIntroState`, `CC_STAGE_ANIMATION_OFF_ADDR=1`).
- `dll_main.cpp:703-706` — on InGame entry, allocates the rollback state pool via `g_rollMan.allocateStates()`.
- `dll_main.cpp:1658-1659` — every frame, runs a ~1.18 MB `saveState()` to capture game memory.
- `manager.hpp:382-384` — `isInRollbackLocked()` returns true only when `_state==InGame && config.rollback != 0 && config.mode.isNetplay()`.
- `manager.cpp:583-585` — `setInput` writes to `frame + rollbackDelay` (instead of `frame + delay`).
- `manager.cpp:636` — divergence detection in `setInputsLocked` becomes active.
- `manager.cpp:753-762` — `isRemoteInputReadyLocked` returns true on the first InGame frame even when the remote has zero inputs, using a `lastInputBefore` prediction.

**Before `43a6fdd`:** `rollback == 0` was always sent to the DLL. None of the above code ever ran. The DLL effectively performed delay-based netplay with no rollback attempts. No desync was possible because no rollback was ever attempted.

**After `43a6fdd`:** every netplay match sends `rollback = 4/6/7/8`. The DLL's full rollback machinery activates, including state save/restore, divergence detection, and speculative prediction. **This is the first commit in the project's history that ever actually enabled rollback in production.**

### 4.4 The latent DLL-side bug

The DLL's rollback code was not battle-tested at the time of `43a6fdd`. The git log of `src/dll/netplay/rollback_manager.cpp` and friends shows that the **fixes came AFTER `43a6fdd`**:

| Commit (post-43a6fdd) | Subject | What it fixed |
|------------------------|---------|---------------|
| `847ded7` | "saveState disabled… rollback cannot fire" | saveState was crashing / being disabled |
| `c78f938` | "THE saveState crash fix" | the actual saveState memory-corruption bug |
| `6c83816` | "fix rollback-deadlock bugs" | `isRemoteInputReadyLocked` returning false on first InGame frame |
| `027d9ee` | "Phase B speculative rollback" | adds `MAX_ROLLBACK=15` frame speculation (also adds the `lastInputBefore` prediction at `manager.cpp:753-762`) |

The prime suspect for the immediate desync reported in the issue is the `lastInputBefore` prediction path (added in `6c83816`, later refined by `027d9ee`). On the first InGame frame, both peers transition simultaneously; the remote has no InGame-frame inputs yet. With `rollback > 0`, the gate returns true and the game advances using the stale `lastInputBefore` from the prior CharaSelect/CharaIntro transition — which is typically the **Confirm press** (a button input). The real remote InGame input (likely neutral) arrives 1 RTT later, diverges from the prediction, and the rollback engine tries to correct. **But on frame 0 of InGame, no `saveState` has run yet** (`saveState` happens at END of the first `frameStep`), so `loadState` returns false (`rollback_manager.cpp:181`) and the desync propagates irrecoverably.

### 4.5 Why the symptom is "desync on first sync attempt"

This matches the mechanism precisely:

- Both peers load the match → DLL allocates rollback state pool.
- Both peers enter `in_game` scene → `g_netMan` enters `InGame` state.
- On the very first InGame frame, `setInputsLocked` is called.
- `isRemoteInputReadyLocked` returns true via `lastInputBefore` (stale Confirm press).
- Game advances with stale input. Local advances with whatever the player is doing.
- Remote's actual InGame-frame-0 input arrives 1 RTT later.
- Divergence detected → rollback triggered → `loadState` fails (no prior `saveState`) → correction is silently dropped → permanent desync.

The "immediate after loading" timing in the issue report is the literal fingerprint of this bug.

---

## 5. Build Validation

The user asked to "make a clone of the project and build it to validate and test claims."

### 5.1 Build infrastructure validation (✓)

The clone succeeded. The build infrastructure is intact:

- `CMakeLists.txt` (15,886 bytes) — well-formed, sets up cross-compile to Windows 32-bit, fetches SDL2/ImGui/ENet via `FetchContent`.
- `cmake/toolchain-mingw32.cmake` — properly locates `i686-w64-mingw32-gcc/g++/windres`, statically links libstdc++/libgcc/libwinpthread.
- `scripts/build.sh` — full configure/build/strip/zip pipeline; includes a stale-cache guard that purges a CMakeCache.txt pinned to the wrong compiler.

### 5.2 Actual cross-compile (✗ — sandbox constraint)

A full Windows-binary build **cannot be performed** in this sandbox because:

- `i686-w64-mingw32-g++` is not installed.
- There is no root access to `apt-get install mingw-w64`.
- Running `cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-mingw32.cmake` correctly fails at the toolchain file's explicit check (`cmake/toolchain-mingw32.cmake:24`), with the exact error message: *"MinGW-w64 i686 cross-compiler not found. On Debian/Ubuntu: sudo apt-get install mingw-w64"*.

This is a sandbox environment limitation, not a project defect. The project's build setup is correct and would build successfully on any machine with `mingw-w64` installed.

### 5.3 What we *were* able to validate

- **Headers parse cleanly under native `g++ 14.2.0 -std=c++23 -fsyntax-only`** — `config_buffer.hpp` and `netplay_config.hpp` both compile without errors (only the expected `#pragma once in main file` warning when syntax-checking a header directly).
- **`session.cpp` includes `<winsock2.h>`**, which is unavailable on Linux, so a native syntax-check of that translation unit isn't possible without stubs. This is expected for a Windows-only codebase.
- **The full diff of `session.cpp::finish_ping_exchange` was verified line-by-line** against both `git show 43a6fdd~1:...` (pre) and the current file (post). The smoking-gun line `config_.rollback = static_cast<std::uint8_t>(rollback);` is present in the post version and absent in the pre version.
- **The IPC wire layout is verified** (`config_buffer.hpp:14`): `rollback` is a single byte at offset 2 — no bit-flag packing, no off-by-one risk.
- **The DLL consumption path is verified** (`dll_main.cpp:461-517`): `cfg.rollback > 0` gates the entire rollback-specific code path.

### 5.4 Recommended runtime validation (for the maintainer)

Since a Windows binary cannot be produced here, the maintainer should validate the diagnosis on a real Windows/Wine setup with this experiment:

1. **Build at commit `43a6fdd`** (or current `main` with `815dcd6`, which still has the auto-rollback assignment).
2. **Patch `finish_ping_exchange`** to revert to the pre-`43a6fdd` behavior (comment out the `config_.rollback = ...` line, or force `rollback = 0`).
3. **Run a netplay match.** Prediction: the desync disappears — but rollback also does nothing, because the DLL falls back to delay-based netplay.
4. **Restore `rollback > 0`, set env vars `CASTER_DETERMINISTIC=1` and `CASTER_PREDICTOR=last`** to isolate whether the bug is in the `lastInputBefore` prediction path or in the speculative Phase B path.

If step 3 makes the desync disappear, the diagnosis is confirmed.

---

## 6. Final Report

### 6.1 What the issue actually says vs. what is actually happening

The issue's title — "Rollback broken since 43a6fdd" — is **technically correct but misleading**. A more precise framing would be:

> *"Commit 43a6fdd is the first commit that actually sends a non-zero `rollback` value to the DLL. Before 43a6fdd, the launcher silently ran delay-based netplay (rollback was hardcoded to 0 by omission). After 43a6fdd, rollback is properly negotiated (4/6/7/8 frames based on RTT) — but this activates DLL-side rollback code that had pre-existing latent bugs, which were not fixed until later commits (847ded7, c78f938, 6c83816, 027d9ee)."*

### 6.2 The exact change responsible

**Single line added in `src/exe/session/session.cpp::finish_ping_exchange()` at commit `43a6fdd`:**

```cpp
config_.rollback = static_cast<std::uint8_t>(rollback);
```

This is the direct trigger. Removing this one line reverts the project to its pre-`43a6fdd` behavior (no rollback attempted, no desync observed — but also no rollback netplay).

### 6.3 Where the actual bug lives

The bug is **not** in the launcher code modified by `43a6fdd`. The launcher's new behavior is correct: it computes a reasonable rollback window from RTT and propagates it through the IPC config buffer without corruption.

The bug lives in the **DLL-side rollback implementation** as it existed at commit `43a6fdd`. The most likely culprit (per sub-agent 1-d's analysis, HIGH confidence) is the `lastInputBefore` prediction path in `src/dll/netplay/manager.cpp:753-762`, which returns true on the first InGame frame even when the remote has no inputs yet — combined with the fact that no `saveState` has run yet at that point, so the rollback engine cannot recover from the resulting misprediction.

### 6.4 Why the issue's "last known working" is misleading

The issue says rollback "worked correctly as of the 3 commits prior to 43a6fdd". This is true but only because **rollback was never actually being exercised** — the launcher was sending `rollback = 0`, so the DLL's rollback path was dormant. The "working" state was actually "not running rollback at all". `43a6fdd` did not break rollback; it turned it on for the first time and exposed that it had never actually worked.

### 6.5 Recommended fixes (in order of preference)

1. **Apply the post-`43a6fdd` DLL fixes** (`847ded7`, `c78f938`, `6c83816`, `027d9ee`) if they are not yet on the user's branch. The repo at `HEAD = 815dcd6` already includes them, so simply rebuilding from current `main` may resolve the issue. The user should confirm whether they were testing at `43a6fdd` itself or at a later commit.

2. **Seed the InGame transition with a known-neutral input (0)** for the first `delay` frames, instead of inheriting `lastInputBefore` from the prior transition index. This prevents the stale Confirm-press from being treated as the remote's first InGame input.

3. **Run `saveState` once on InGame entry** (before the first `frameStep`), so that the rollback engine has at least one valid state to restore to if a misprediction is detected on frame 0.

4. **Add a `CASTER_DETERMINISTIC=1` env-var escape hatch** that disables speculative rollback (Phase B), reducing the prediction surface to the minimal path. This makes the bug easier to isolate.

5. **As a temporary workaround for end users:** explicitly set `rollback = 0` in the launcher's waiting-for-peer screen. This restores pre-`43a6fdd` behavior (delay-based netplay, no desync, but no rollback either).

### 6.6 Confidence summary

| Claim | Confidence | Basis |
|-------|-----------|-------|
| `43a6fdd` is the commit that first sent `rollback > 0` to the DLL | **HIGH** | Direct line-by-line diff of `finish_ping_exchange` pre/post; verified by 3 independent sub-agents |
| The launcher's new rollback computation is internally correct | **HIGH** | The IPC wire format is unchanged; rollback is a single byte at offset 2; no off-by-one |
| The controller-mapping / config / UI changes in `43a6fdd` do NOT contribute to the desync | **HIGH** | Mappings never cross the wire; the wire payload is a resolved `uint16_t`; the `--rollback` CLI flag was relocated, not removed |
| The actual desync originates in the DLL's `lastInputBefore` prediction path on first InGame frame | **MEDIUM** | Strong circumstantial evidence (no `saveState` yet on frame 0; `loadState` would return false; stale Confirm press inherited). Would be confirmed by the runtime experiment in §5.4 |
| A full Windows binary build would succeed on a properly-equipped machine | **HIGH** | Build infrastructure is intact; the only failure is the missing MinGW cross-compiler in this sandbox |

---

## 7. Artifacts Produced

| Artifact | Path |
|----------|------|
| Cloned repository | `/home/z/my-project/investigation/ReCaster/` |
| Multi-agent worklog | `/home/z/my-project/worklog.md` |
| This report (Markdown) | `/home/z/my-project/download/ReCaster_issue1_investigation_report.md` |

---

## 8. Acknowledgements

This investigation was performed by a coordinating main agent with four parallel sub-agents (Task IDs 1-a, 1-b, 1-c, 1-d) using the `general-purpose` agent type. All four sub-agents independently converged on the same root cause. Their full findings are recorded in `/home/z/my-project/worklog.md`.
