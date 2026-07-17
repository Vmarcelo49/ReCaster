#!/usr/bin/env bash
# scripts/desync_test.sh
#
# Launch two caster.exe instances (host + joiner) on localhost with
# auto-mash enabled, then watch both DLL trace logs for desyncs.
#
# This is the diagnostic harness for GitHub issue #1
# (rollback desync after 43a6fdd).
#
# ----------------------------------------------------------------------------
# Usage:
#   ./scripts/desync_test.sh                  # 60s default, random pattern
#   ./scripts/desync_test.sh 30               # 30s
#   ./scripts/desync_test.sh 60 46320         # 60s, custom port
#   CASTER_AUTO_INPUT_PATTERN=collide ./scripts/desync_test.sh
#
# Env knobs (all optional):
#   CASTER_AUTO_INPUT=1            default 1 — auto-mash to advance menus
#   CASTER_AUTO_INPUT_PATTERN      default random — diverge/collide/idle/random
#                                  (random = same pseudo-random seq on both
#                                   peers, forces fast divergence)
#   CASTER_SYNCHASH_INTERVAL=N     default 30 — frames between SyncHash probes
#                                  (lower = catch desyncs faster, more CPU)
#   CASTER_LOG_REMOTE_INPUTS=1     default unset — extremely verbose, only
#                                  use to diagnose protocol-layer issues
#   CASTER_DETERMINISTIC=1         default unset — disable Phase B speculative
#                                  rollback (uses config.rollback as cap)
#   RECASTER_MBAACC_DIR            default autodetect from script location
#   RECASTER_PORT                  default 46318
#   RECASTER_TIMEOUT_SECS          default 60
#   RECASTER_WINE                  default wine
# ----------------------------------------------------------------------------

set -euo pipefail

# ----------------------------------------------------------------------------
# Argument + env resolution
# ----------------------------------------------------------------------------

TIMEOUT_SECS="${1:-${RECASTER_TIMEOUT_SECS:-60}}"
PORT="${2:-${RECASTER_PORT:-46318}}"
WINE_BIN="${RECASTER_WINE:-wine}"

# Locate the MBAACC folder relative to this script.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

if [[ -n "${RECASTER_MBAACC_DIR:-}" ]]; then
    MBAACC_DIR="$RECASTER_MBAACC_DIR"
else
    MBAACC_DIR="/home/marcelo/Downloads/Community_Edition_3-1-004/MBAACC - Community Edition/MBAACC"
fi

CASTER_EXE="$MBAACC_DIR/caster.exe"
CasterCFG_DIR="$MBAACC_DIR/caster"
HLOG="$CasterCFG_DIR/host_debug.log"
JLOG="$CasterCFG_DIR/join_debug.log"
DLOG="$MBAACC_DIR/debug.log"

# Diagnostics env vars passed to both wine processes.
export CASTER_AUTO_INPUT="${CASTER_AUTO_INPUT:-1}"
# Default pattern = random (forces fast divergence + collisions)
export CASTER_AUTO_INPUT_PATTERN="${CASTER_AUTO_INPUT_PATTERN:-random}"
export CASTER_SYNCHASH_INTERVAL="${CASTER_SYNCHASH_INTERVAL:-30}"
# Default: do NOT set CASTER_LOG_REMOTE_INPUTS — too noisy (10k+ lines/run)
# Keep Wine quiet (fixme channel is noisy).
export WINEDEBUG="${WINEDEBUG:-fixme-all}"

# ----------------------------------------------------------------------------
# Pre-flight
# ----------------------------------------------------------------------------

echo "================================================================"
echo "ReCaster desync test harness (issue #1)"
echo "================================================================"
echo "MBAACC dir : $MBAACC_DIR"
echo "caster.exe : $CASTER_EXE"
echo "Port       : $PORT"
echo "Timeout    : ${TIMEOUT_SECS}s"
echo "Wine       : $WINE_BIN ($($WINE_BIN --version 2>&1 | head -1))"
echo "Env        : CASTER_AUTO_INPUT=$CASTER_AUTO_INPUT"
echo "           : CASTER_AUTO_INPUT_PATTERN=$CASTER_AUTO_INPUT_PATTERN"
echo "           : CASTER_SYNCHASH_INTERVAL=$CASTER_SYNCHASH_INTERVAL"
echo "           : CASTER_LOG_REMOTE_INPUTS=${CASTER_LOG_REMOTE_INPUTS:-<unset>}"
echo "           : CASTER_DETERMINISTIC=${CASTER_DETERMINISTIC:-<unset>}"
echo "           : WINEDEBUG=$WINEDEBUG"
echo "================================================================"

if [[ ! -f "$CASTER_EXE" ]]; then
    echo "ERROR: caster.exe not found at $CASTER_EXE"
    echo "       Build first: cd $REPO_DIR && ./scripts/build.sh"
    exit 2
fi
if [[ ! -f "$MBAACC_DIR/hook.dll" ]]; then
    echo "ERROR: hook.dll not found in $MBAACC_DIR"
    exit 2
fi
if ! command -v "$WINE_BIN" >/dev/null; then
    echo "ERROR: $WINE_BIN not found in PATH"
    exit 2
fi

# ----------------------------------------------------------------------------
# Clean up previous run
# ----------------------------------------------------------------------------

echo "[setup] cleaning previous logs..."
rm -f "$HLOG" "$JLOG"
: > "$DLOG" 2>/dev/null || true

pkill -f "caster.exe --host" 2>/dev/null || true
pkill -f "caster.exe --join" 2>/dev/null || true
pkill -f "MBAA.exe" 2>/dev/null || true
sleep 1

# ----------------------------------------------------------------------------
# Launch HOST
# ----------------------------------------------------------------------------

echo "[launch] starting HOST on port $PORT..."
cd "$MBAACC_DIR"
$WINE_BIN "$CASTER_EXE" --host --port="$PORT" --name=HOST_TEST \
    > "$MBAACC_DIR/host_stdout.log" 2>&1 &
HOST_PID=$!
echo "[launch] HOST PID=$HOST_PID (wine wrapper)"

echo "[launch] waiting 6s for host to start listening..."
sleep 6

# ----------------------------------------------------------------------------
# Launch JOINER
# ----------------------------------------------------------------------------

echo "[launch] starting JOINER -> 127.0.0.1:$PORT..."
$WINE_BIN "$CASTER_EXE" --join="127.0.0.1:$PORT" --name=JOIN_TEST \
    > "$MBAACC_DIR/join_stdout.log" 2>&1 &
JOIN_PID=$!
echo "[launch] JOINER PID=$JOIN_PID (wine wrapper)"

# ----------------------------------------------------------------------------
# Watchdog loop
# ----------------------------------------------------------------------------

cleanup() {
    local rc=$?
    echo "[cleanup] killing test processes..."
    for pid in $HOST_PID $JOIN_PID; do
        kill -TERM "$pid" 2>/dev/null || true
    done
    sleep 2
    pkill -KILL -f "caster.exe" 2>/dev/null || true
    pkill -KILL -f "MBAA.exe" 2>/dev/null || true
    exit $rc
}
trap cleanup INT TERM

start_time_ms=$(($(date +%s%N) / 1000000))
host_ingame_ts=""
join_ingame_ts=""
desync_ts=""
desync_side=""
desync_detail=""

elapsed_sec() {
    local now_ms=$(($(date +%s%N) / 1000000))
    local diff_ms=$((now_ms - start_time_ms))
    # Print as float (1 decimal place) — bash arithmetic can't do floats,
    # so we synthesize "N.M" by dividing by 100 and inserting a dot.
    local whole=$((diff_ms / 1000))
    local frac=$((diff_ms % 1000 / 100))
    printf "%d.%d" "$whole" "$frac"
}

echo "[watch] watching for InGame entry, DESYNC, rollback events, or timeout (${TIMEOUT_SECS}s)..."

while true; do
    elapsed_ms=$(( $(date +%s%N) / 1000000 - start_time_ms ))
    elapsed=$((elapsed_ms / 1000))

    if (( elapsed >= TIMEOUT_SECS )); then
        echo "[watch] TIMEOUT reached after ${elapsed}s"
        break
    fi

    if ! kill -0 "$HOST_PID" 2>/dev/null && ! kill -0 "$JOIN_PID" 2>/dev/null; then
        echo "[watch] both processes exited after ${elapsed}s"
        break
    fi

    # InGame entry (one-shot).
    if [[ -z "$host_ingame_ts" && -f "$HLOG" ]] \
       && grep -qE "state-transition .*->InGame" "$HLOG" 2>/dev/null; then
        host_ingame_ts="$(elapsed_sec)"
        echo "[watch] HOST entered InGame at t=${host_ingame_ts}s"
    fi
    if [[ -z "$join_ingame_ts" && -f "$JLOG" ]] \
       && grep -qE "state-transition .*->InGame" "$JLOG" 2>/dev/null; then
        join_ingame_ts="$(elapsed_sec)"
        echo "[watch] JOIN entered InGame at t=${join_ingame_ts}s"
    fi

    # DESYNC detection — check both the shared debug.log and the per-side
    # trace logs for the desync event we now write. Poll every 200ms so we
    # don't miss a desync that fires shortly before the processes exit.
    if [[ -f "$DLOG" ]] && grep -qE "DESYNC detected|delayedStop — Desync" "$DLOG" 2>/dev/null; then
        desync_ts="$(elapsed_sec)"
        desync_detail=$(grep -E "DESYNC detected|delayedStop — Desync" "$DLOG" | head -3)
        if [[ -f "$HLOG" ]] && grep -qE "^EVENT desync" "$HLOG" 2>/dev/null; then
            desync_side="host"
        elif [[ -f "$JLOG" ]] && grep -qE "^EVENT desync" "$JLOG" 2>/dev/null; then
            desync_side="join"
        else
            desync_side="unknown"
        fi
        echo "[watch] *** DESYNC DETECTED at t=${desync_ts}s (side=$desync_side) ***"
        echo "[watch] detail: $desync_detail"
        break
    fi

    sleep 0.2
done

final_time_ms=$(($(date +%s%N) / 1000000))
total_elapsed_ms=$((final_time_ms - start_time_ms))
total_elapsed_whole=$((total_elapsed_ms / 1000))
total_elapsed_frac=$((total_elapsed_ms % 1000 / 100))
total_elapsed="${total_elapsed_whole}.${total_elapsed_frac}"

# Kill survivors
for pid in $HOST_PID $JOIN_PID; do
    kill -TERM "$pid" 2>/dev/null || true
done
sleep 2
pkill -KILL -f "caster.exe" 2>/dev/null || true
pkill -KILL -f "MBAA.exe" 2>/dev/null || true

# ----------------------------------------------------------------------------
# Summary
# ----------------------------------------------------------------------------

echo ""
echo "================================================================"
echo "TEST SUMMARY  (total elapsed: ${total_elapsed}s)"
echo "================================================================"

# InGame entry
if [[ -n "$host_ingame_ts" ]]; then
    echo "  HOST entered InGame : YES (t=${host_ingame_ts}s)"
else
    echo "  HOST entered InGame : NO"
fi
if [[ -n "$join_ingame_ts" ]]; then
    echo "  JOIN entered InGame : YES (t=${join_ingame_ts}s)"
else
    echo "  JOIN entered InGame : NO"
fi

# Desync detection
if [[ -n "$desync_ts" ]]; then
    echo "  DESYNC detected     : YES (t=${desync_ts}s, side=$desync_side)"
    echo "  detail              : $desync_detail"
else
    echo "  DESYNC detected     : NO (no SyncHash mismatch within ${total_elapsed}s)"
fi

# SyncHash exchange count (how many matches were logged)
if [[ -f "$DLOG" ]]; then
    sync_matches=$(grep -cE "SyncHash MATCH" "$DLOG" 2>/dev/null || echo 0)
    echo "  SyncHash matches    : $sync_matches  (0 = no hashes exchanged = bug signal)"
fi

# Per-side stats
for side in host join; do
    if [[ "$side" == "host" ]]; then
        log="$HLOG"
    else
        log="$JLOG"
    fi
    if [[ ! -f "$log" ]]; then
        echo "  $side log            : MISSING"
        continue
    fi
    rb_triggers=$(grep -cE "^EVENT rollback-trigger" "$log" 2>/dev/null || echo 0)
    rb_load_ok=$(grep -cE "^EVENT rollback-load-ok" "$log" 2>/dev/null || echo 0)
    rb_load_fail=$(grep -cE "loadState FAILED|loadState failed" "$log" 2>/dev/null || echo 0)
    state_transitions=$(grep -cE "^EVENT state-transition" "$log" 2>/dev/null || echo 0)
    last_frame_line=$(grep -E "^(i[0-9]+ )?F [0-9]+ \| st=" "$log" 2>/dev/null | tail -1)
    last_state=""
    last_idx=""
    last_frm=""
    last_rmt_idx=""
    last_rmt_frm=""
    last_lcf_idx=""
    last_lcf_frm=""
    if [[ -n "$last_frame_line" ]]; then
        last_state=$(echo "$last_frame_line" | sed -E 's/.*st=([A-Za-z]+).*/\1/')
        last_idx=$(echo "$last_frame_line" | sed -E 's/.*idx=([0-9]+).*/\1/' 2>/dev/null)
        last_frm=$(echo "$last_frame_line" | sed -E 's/.*st=[A-Za-z]+ idx=[0-9]+ frm=([0-9]+).*/\1/' 2>/dev/null)
        last_rmt_idx=$(echo "$last_frame_line" | sed -E 's/.*rmt:idx=([0-9]+).*/\1/' 2>/dev/null)
        last_rmt_frm=$(echo "$last_frame_line" | sed -E 's/.*rmt:idx=[0-9]+ frm=([0-9]+).*/\1/' 2>/dev/null)
        last_lcf_idx=$(echo "$last_frame_line" | sed -E 's/.*lcf=([0-9]+)\/.*/\1/' 2>/dev/null)
        last_lcf_frm=$(echo "$last_frame_line" | sed -E 's/.*lcf=[0-9]+\/([0-9]+).*/\1/' 2>/dev/null)
    fi
    echo "  $side rollbacks      : trigger=$rb_triggers load_ok=$rb_load_ok load_fail=$rb_load_fail"
    echo "  $side state changes  : $state_transitions"
    echo "  $side last frame     : st=$last_state idx=$last_idx frm=$last_frm | rmt:idx=$last_rmt_idx frm=$last_rmt_frm | lcf=$last_lcf_idx/$last_lcf_frm"
done

echo ""
echo "Log files:"
echo "  host trace : $HLOG"
echo "  join trace : $JLOG"
echo "  shared log : $DLOG"
echo "================================================================"

# Exit code: 0 = no desync detected, 1 = desync detected, 2 = setup error.
if [[ -n "$desync_ts" ]]; then
    exit 1
fi
exit 0
