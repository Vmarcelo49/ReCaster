# AGENTS.md

Guidance for AI coding agents working on ReCaster. Keep changes minimal,
and validate netplay behavior with `scripts/nettest.sh` whenever you touch
session, rollback, or networking code.

## Project overview

- ReCaster is a C++23 rewrite of CCCaster: a rollback-netplay client for
  Melty Blood Actress Again Current Code (`MBAA.exe` 1.07 Rev.1.4.0).
- Ships as two Windows binaries, cross-compiled from Linux:
  - `caster.exe` — SDL2 + ImGui launcher (GUI and CLI modes)
  - `hook.dll` — payload injected into `MBAA.exe`; implements the
    GGPO-style rollback engine (frame hooks, input injection, save/load
    state, RNG sync, SyncHash desync detection)
  - shared code lives in the `caster_common` CMake target
- A Go relay server (`server/`) provides room codes and NAT hole-punching.
  Defaults: relay `zzcaster.duckdns.org:3939`, game UDP port `46318`.

## Repository layout

```
src/exe      launcher: GUI pages, CLI, session FSM, game runner, injector
src/dll      injected payload: entry/game/hooks/input/ipc/memory/
             netplay/protocol/overlay/spec/util
src/common   config, logger, ENet transport + relay client (shared)
server/      Go relay server
scripts/     workflow scripts (see below); smoke_test_*.cpp are standalone
docs/        deep-dive status/design docs (Portuguese-heavy)
MBAACC/      local game folder (gitignored) — deploy target; never commit
             anything from it
```

No CI and no linter/formatter are configured. Match the style of the
surrounding code.

## Dev environment tips

- Toolchain: `i686-w64-mingw32-g++` (MinGW-w64), `cmake >= 3.20`, `zip`,
  `wine` to run. All third-party deps come via CMake FetchContent.
- `./scripts/build.sh [rebuild]` — configure + build + strip + zip into
  `release/caster.zip`. `rebuild` skips reconfiguration.
- If headers like `windows.h` fail with the *host* g++, a stale non-MinGW
  `build/CMakeCache.txt` exists — delete `build/` (full-mode `build.sh`
  guards this automatically).
- `./scripts/deploy.sh [quick|full|only]` — build (unless `only`) and copy
  `caster.exe` + `hook.dll` (+ `d3d9.dll` DXVK if absent) into `MBAACC/`.
  It refuses to overwrite while any `caster.exe`/`MBAA.exe` instance runs.

## Environment knobs

All script/DLL env vars in one place. Scripts default their game folder
to `<repo>/MBAACC` unless overridden.

| Scope | Variable | Default | Purpose |
|---|---|---|---|
| Build (`build.sh`) | `CASTER_BUILD_TYPE` | `Release` | CMake build type |
| Build | `CASTER_BUILD_JOBS` | `nproc` | Parallel compile jobs |
| Build | `STRIP` | `i686-w64-mingw32-strip` | Strip binary after link |
| Deploy/scripts target | `RECASTER_GAME_DIR` | `<repo>/MBAACC` | Game folder used by deploy/vrun/nettest/spectest/watch-logs |
| Deploy | `FORCE_DEPLOY=1` | off | Allow overwrite while a game instance runs |
| Headless displays | `VRUN_DISPLAY` | per script | Wayland socket name (`vrun`: `recaster-virt`, nettest/spectest: own names) |
| Headless displays | `VRUN_SIZE` | `1280x800` | Virtual framebuffer size (`vrun.sh` only) |
| Headless displays | `VRUN_KEEP=1` | off | Leave the display running after the app exits |
| Spectest diagnostics | `SPECTEST_TRACE=1` | off | Per-batch input fingerprints + SEND/RECV PlayerInputs traces |
| DLL runtime | `CASTER_AUTO_INPUT=1` | off | Synthetic input drives menus/matches without a human |
| DLL runtime | `CASTER_AUTO_INPUT_PATTERN` | `diverge` | InGame mash pattern: `diverge`/`collide`/`idle`/`random` |
| DLL runtime | `CASTER_SPECTATE_FASTFWD=1` | off | Spectator joins at fast-forward speed instead of watching from chara select (SPACE toggles in-game either way) |
| DLL runtime | `CASTER_SYNCHASH_INTERVAL` | `150` | Frames between SyncHash checks (30 or 1 = faster desync detection) |
| DLL runtime | `CASTER_LOG_REMOTE_INPUTS=1` | off | Log every PlayerInputs send/receive |
| DLL runtime | `CASTER_LOG_RNG=1` | off | Verbose RNG state logging around save/load |
| DLL runtime | `CASTER_PREDICTOR=stateful` | off | Alternate remote-input predictor |
| DLL runtime (sim) | `CASTER_SIM_LAG_MS` / `_JITTER_MS` / `_LOSS_PCT` / `_SIM_SEED` | off | Network simulator — inject lag/jitter/loss for desync repro (`network_simulator.cpp`) |

DLL runtime vars are set on the `caster.exe` environment and are
inherited by the launched `MBAA.exe`.

## Running & testing

- All CLI flags: `wine MBAACC/caster.exe --help`
  (`--host`, `--join=host:port|#room`, `--spec=…`, `--training`,
  `--versus`, `--port=N`, `--delay=0..8`, `--rollback=0..20`, `--name=X`).
- **Headless runs** (nothing appears on the desktop): `scripts/vrun.sh`
  wraps caster.exe inside a `kwin_wayland --virtual` framebuffer via
  Wine's Wayland driver (DISPLAY unset, WAYLAND_DISPLAY=<socket>).
  `vrun.sh --stop` tears it down; `VRUN_KEEP=1` keeps it for launching
  several instances manually.
- **Netplay regression**: `scripts/nettest.sh [--duration=S] [--rollback=N]
  [--delay=N] [--port=P] [--skip-build] [--with-relay] [--keep-display]`
  - boots host + joiner over localhost, waits until two `MBAA.exe`
    processes exist, monitors stability, counts fresh desync/mismatch
    lines in the DLL logs, then kills everything; exit 0 = PASS.
  - by default it temporarily sets `[network] relays=#` (direct-only);
    use `--with-relay` when touching relay paths.
  - on failure, launcher stdout logs are kept at
    `/tmp/recaster-nettest-{host,join}.*.log`.
- Watch live netplay logs: `scripts/watch-logs.sh [--all] [-n LINES]
  [--host|--join]`.
- Log locations: DLL netplay logs sit next to the game in
  `MBAACC/caster/{host,join}_debug.log`; the launcher logs to stdout when
  `[system] log_to_stdout=true` in `MBAACC/caster/config.ini`.

## Architecture invariants (read before touching netplay code)

- `frameStep()` in `src/dll/entry/dll_main.cpp` implements the rollback
  pipeline and the ordering there is load-bearing (saveState BEFORE
  writeGameInput, stale-SyncHash invalidation after rollback, SyncHash
  gating on fast-forward state, clearLastChangedFrame disabled). Consult
  the step table in `docs/port-status.md` before editing it.
- Threading model: the launcher runs `NetplaySession` and `GameRunner` on
  worker threads fed by command queues (`*_async` methods) and read
  through mutex-guarded snapshots — never mutate session state from
  outside the worker thread. The DLL owns a dedicated network thread that
  holds the `ENetHost*`.
- ENet queues `EVENT_TYPE_CONNECT` exactly once per peer. Every poll site
  must route a `Connected` event onward, never discard it — dropping it
  in a helper pump made direct joins die with "Version exchange timed
  out" (issue #6, fixed in `accept_direct_connect()`).
- `src/dll/game/addresses.hpp` hardcodes MBAA.exe memory offsets;
  everything there is game-version-specific.
- Rollback/desync history: `docs/implementing-real-rollback.md`,
  `docs/ReCaster_issue1_investigation_report.md`.

## Gotchas

- Wine lists processes with Windows-style paths (`Z:\...\MBAACC\caster.exe`),
  so `pgrep`/`pkill` patterns must match backslashes, e.g.
  `'MBAACC[\\/]caster\.exe'`. Also don't repeat such a literal elsewhere in
  the same shell command line, or pkill will match and kill your own shell.
- `grep -c` prints `0` AND exits nonzero on zero matches — don't add a
  redundant `|| echo 0` (you'll get `"0\n0"`).
- Overwriting a running exe/dll under Wine misbehaves; stop instances
  before redeploying.
- `sudo` needs a password: if a step truly requires root (e.g. installing
  packages), stop and hand the exact command to the user instead of
  working around it.
- Xvfb is not installed; use `kwin_wayland --virtual` headless displays
  (see `scripts/vdisplay.sh`) instead.

## Commit instructions

- **MANDATORY — full diff review before EVERY commit (`git diff` and
  `git diff --cached`). This step cannot be skipped, no matter how small
  the change.** Review every changed file for:
  - **Unused code** — dead branches, unused includes/members/functions
    left behind by the edit.
  - **Debug helpers that are no longer useful** — temporary log lines,
    traces, fingerprints, counters added while investigating; remove
    them or gate them behind an env flag if genuinely valuable.
  - **Repeated logic** — duplicated blocks that should be unified into
    one helper or loop.
  - **Verbose and useless comments** — comments narrating obvious code,
    stale references to removed code, or long-winded explanations of
    what the diff already shows.
  Fix everything found (or explicitly justify keeping it) before
  committing. A skipped review is a process violation.
- Trunk-based development on `main`; validate behavior before pushing
  (`scripts/nettest.sh` / `scripts/spectest.sh` for netplay changes).
- Conventional commits with scope: `fix(session): …`, `feat(sfx): …`,
  `docs(port-status): …`, `refactor: …`.
- Reference issues as `#N`; `Fixes #N` in the body auto-closes on push.
- Never commit: `MBAACC/`, `build/`, `release/`, caster configs/logs
  (gitignored), or scratch files like `scripts/commit_*.txt`.

## Where deeper docs live

- `docs/port-status.md` — port phases, validated features, known issues
- `docs/dll-architecture.md` — hook.dll module map
- `docs/threading-migration.md` — threading layers (launcher + DLL)
- `docs/spectator-plan.md` — spectator mode plan/validation matrix
- `docs/future-improvements.md` — prioritized feature backlog
