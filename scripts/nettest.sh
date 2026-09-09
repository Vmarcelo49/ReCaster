#!/usr/bin/env bash
# nettest — spin up a local online match: host + join over localhost.
#
# Launches TWO caster.exe instances on the hidden virtual display
# (see vdisplay.sh), host on --port, joiner via --join=127.0.0.1:port,
# both games booting under Wine. Watches them for the requested duration,
# reports stability + fresh desync/mismatch lines from the DLL logs,
# then cleans everything up (games, launchers, virtual display).
#
# Note: in CLI mode the JOINER launcher exits right after launching its
# game (the injected hook.dll owns the connection from there), so success
# is measured by both MBAA.exe processes staying alive, not launcher PIDs.
#
# Usage:
#   ./scripts/nettest.sh [options]          (= or space-separated values)
#     --duration=S   seconds to keep the match running   (default: 45)
#     --rollback=N   rollback window frames              (default: 4)
#     --delay=N      input delay frames                  (default: 1)
#     --port=P       host UDP port                       (default: 46318)
#     --skip-build   don't run deploy.sh first
#     --keep-display leave the virtual display running afterwards
#     --with-relay   keep your [network] relays config (default: overridden)
#
# RELAY NOTE: by default this script sets [network] relays=# (empty list)
# so the host runs DIRECT-ONLY. With a relay client armed, the ENet
# intercept ("sole-socket-reader mode") eats the joiner's Version packet
# and localhost joins die with "Version exchange timed out". Pass
# --with-relay to test relay behaviour instead.
#
# Exit code: 0 if the match survived the whole duration without desync
# signatures, 1 otherwise.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
GAME_DIR="${RECASTER_GAME_DIR:-${ROOT_DIR}/MBAACC}"
LOG_DIR="${GAME_DIR}/caster"

DURATION=45; ROLLBACK=4; DELAY=1; PORT=46318
SKIP_BUILD=0; KEEP_DISPLAY=0; WITH_RELAY=0
SIM_SPEC=""; SIM_SWEEP=0
while [ $# -gt 0 ]; do
    arg="$1"
    case "$arg" in
        --skip-build)   SKIP_BUILD=1; shift; continue ;;
        --keep-display) KEEP_DISPLAY=1; shift; continue ;;
        --with-relay)   WITH_RELAY=1; shift; continue ;;
        --sim-sweep)    SIM_SWEEP=1; shift; continue ;;
        --sim|--sim=*|--duration|--duration=*|--rollback|--rollback=*|--delay|--delay=*|--port|--port=*)
            if [[ "$arg" == *=* ]]; then       # --flag=value
                val="${arg#*=}"; shift
            else                               # --flag value
                val="${2:-}"; shift 2
            fi
            case "${arg%%=*}" in
                --sim)      SIM_SPEC="$val" ;;
                --duration) DURATION="$val" ;;
                --rollback) ROLLBACK="$val" ;;
                --delay)    DELAY="$val" ;;
                --port)     PORT="$val" ;;
            esac
            ;;
        *) echo "nettest: unknown option '$arg'" >&2; exit 2 ;;
    esac
done

for v in "$DURATION" "$ROLLBACK" "$DELAY" "$PORT"; do
    [[ "$v" =~ ^[0-9]+$ ]] || { echo "nettest: '$v' is not a number" >&2; exit 2; }
done

# --- simulator spec: --sim="lag=N,jitter=N,loss=N[,seed=N]" ---------------
# Plumbs the existing CASTER_SIM_* env vars (network_simulator.cpp) into
# both instances. Empty fields default to 0 (= disabled).
SIM_LAG=""; SIM_JITTER=""; SIM_LOSS=""; SIM_SEED=""
if [ -n "$SIM_SPEC" ]; then
    IFS=',' read -ra _sim_parts <<< "$SIM_SPEC"
    for _kv in "${_sim_parts[@]}"; do
        case "$_kv" in
            lag=*)    SIM_LAG="${_kv#lag=}" ;;
            jitter=*) SIM_JITTER="${_kv#jitter=}" ;;
            loss=*)   SIM_LOSS="${_kv#loss=}" ;;
            seed=*)   SIM_SEED="${_kv#seed=}" ;;
            *) echo "nettest: unknown --sim field '$_kv' (want lag=/jitter=/loss=/seed=)" >&2; exit 2 ;;
        esac
    done
fi
SIM_LAG="${SIM_LAG:-0}"; SIM_JITTER="${SIM_JITTER:-0}"; SIM_LOSS="${SIM_LOSS:-0}"
for v in "$SIM_LAG" "$SIM_JITTER" "$SIM_LOSS" $SIM_SEED; do
    [ -z "$v" ] && continue
    [[ "$v" =~ ^[0-9]+$ ]] || { echo "nettest: sim value '$v' is not a number" >&2; exit 2; }
done
SIM_ACTIVE=0
{ [ "$SIM_LAG" -gt 0 ] || [ "$SIM_JITTER" -gt 0 ] || [ "$SIM_LOSS" -gt 0 ]; } && SIM_ACTIVE=1

source "${SCRIPT_DIR}/vdisplay.sh"
DISPLAY_NAME="${VRUN_DISPLAY:-recaster-nettest}"
WIDTH=1280; HEIGHT=800

HOST_OUT="$(mktemp /tmp/recaster-nettest-host.XXXX.log)"
JOIN_OUT="$(mktemp /tmp/recaster-nettest-join.XXXX.log)"

log()  { echo "[nettest] $*"; }
fail() { echo "[nettest] FAIL: $*" >&2; exit 1; }

# Wine shows exe paths Windows-style (Z:\dir\MBAACC\caster.exe); match
# both separators so pkill/guards work regardless.
EXES="${GAME_NAME:-$(basename "$GAME_DIR")}[\\\\/](caster|MBAA)\\.exe"

procs()      { pgrep -f "$EXES" 2>/dev/null | wc -l; }
mbaa_count() { pgrep -f "$(basename "$GAME_DIR")[\\\\/]MBAA\\.exe" 2>/dev/null | wc -l; }

count_matches() { # <file> <pattern> -> match count ("0" if file missing)
    local f="$1" pat="$2" out=""
    if [ -f "$f" ]; then
        out=$(grep -ciE "$pat" "$f" 2>/dev/null) || true
    fi
    printf '%s' "${out:-0}"
}

# --- simulator preset sweep (Stage 11.1) -----------------------------------
# Runs the standard bad-connection preset table as sequential single-preset
# nettests and prints one pass/fail summary. Each preset is a FULL child
# nettest (its own display + config override/restore + cleanup), so this
# parent must run BEFORE override_config/trap (it exits here without
# touching the user's relay config).
#
# The preset table (lag/jitter/loss) is the tunable gate: the 0/0/0 baseline
# must always pass; the rest define the tested rollback robustness envelope.
SIM_PRESETS=("0/0/0" "5/1/1" "30/15/5" "80/40/10" "150/60/20")

run_sweep() {
    local base_seed="${SIM_SEED:-1}"
    local pass=0 fail=0 table="" p lag jit loss out
    log "sim-sweep: ${#SIM_PRESETS[@]} presets, base seed=$base_seed, rollback=$ROLLBACK delay=$DELAY"
    for p in "${SIM_PRESETS[@]}"; do
        IFS='/' read -r lag jit loss <<<"$p"
        log "sweep [$p]: lag=$lag jitter=$jit loss=$loss"
        out="/tmp/recaster-sweep-${lag}-${jit}-${loss}.log"
        # Child re-runs the full nettest for one preset. --skip-build avoids
        # a rebuild per preset; the sim is applied to both instances by the
        # child (with auto-input so the match actually reaches InGame).
        local child=(--sim="lag=$lag,jitter=$jit,loss=$loss,seed=$base_seed" --skip-build
                     --duration="$DURATION" --rollback="$ROLLBACK" --delay="$DELAY" --port="$PORT")
        [ "$WITH_RELAY" = "1" ] && child+=(--with-relay)
        if "$0" "${child[@]}" >"$out" 2>&1; then
            table+="PASS  $p"$'\n'; pass=$((pass+1))
        else
            table+="FAIL  $p"$'\n'; fail=$((fail+1))
        fi
        sleep 2   # let wineserver/display settle between presets
    done
    echo
    echo "==================== SIM SWEEP RESULT ===================="
    printf "%s" "$table"
    echo "----------------------------------------------------------------"
    echo "pass=$pass fail=$fail   (per-preset logs: /tmp/recaster-sweep-*.log)"
    echo "==========================================================="
    [ "$fail" -eq 0 ]
}

if [ "$SIM_SWEEP" = "1" ]; then
    run_sweep
    exit $?
fi

desync_pat='desync|mismatch|sync.*fail'
host_desync_before=$(count_matches "${LOG_DIR}/host_debug.log" "$desync_pat")
join_desync_before=$(count_matches "${LOG_DIR}/join_debug.log" "$desync_pat")

# --- local-test config overrides (relays off; verbose stdout) ----------------
CONFIG_INI="${LOG_DIR}/config.ini"
CONFIG_BAK=""
override_config() {
    [ "$WITH_RELAY" = "1" ] && return 0
    [ -f "$CONFIG_INI" ] || return 0
    CONFIG_BAK="${CONFIG_INI}.nettest-bak"
    cp "$CONFIG_INI" "$CONFIG_BAK"
    if grep -q '^relays=' "$CONFIG_INI"; then
        sed -i 's/^relays=.*/relays=#/' "$CONFIG_INI"
    else
        printf '\nrelays=#\n' >> "$CONFIG_INI"
    fi
    log "config: relays overridden to '#' (direct-only) — restored on exit"
}
restore_config() {
    if [ -n "$CONFIG_BAK" ] && [ -f "$CONFIG_BAK" ]; then
        mv -f "$CONFIG_BAK" "$CONFIG_INI"
        CONFIG_BAK=""
    fi
}
override_config

cleanup() {
    log "cleaning up..."
    restore_config
    pkill -f "$EXES" 2>/dev/null || true
    sleep 3
    if [ "$(procs)" -gt 0 ]; then
        log "stray wine processes remain — wineserver -k"
        wineserver -k 2>/dev/null || true
        sleep 1
    fi
    if [ "$KEEP_DISPLAY" != "1" ]; then
        vdisp_stop "$DISPLAY_NAME"
    fi
    # stdout logs intentionally kept in /tmp for post-mortem
}
trap cleanup EXIT
trap 'exit 143' INT TERM

# --- preflight ---------------------------------------------------------------
for f in wine kwin_wayland ss; do
    command -v "$f" >/dev/null || fail "required binary not found: $f"
done
[ -f "${GAME_DIR}/caster.exe" ] || fail "${GAME_DIR}/caster.exe missing — run ./scripts/deploy.sh"
if [ "$(procs)" -gt 0 ]; then
    fail "another caster.exe/MBAA.exe instance is running — close it first"
fi

# --- build + display ---------------------------------------------------------
if [ "$SKIP_BUILD" != "1" ]; then
    "$SCRIPT_DIR/deploy.sh" quick
fi

vdisp_ensure "$DISPLAY_NAME" "$WIDTH" "$HEIGHT" || fail "virtual display failed to start"

run_caster() { # <logfile> <instance_seed> <args...>
    local logfile="$1"; shift
    local iseed="$1"; shift
    local envargs=(-u DISPLAY WAYLAND_DISPLAY="$DISPLAY_NAME" WINEDEBUG=fixme-all
                  CASTER_SIM_LAG_MS="$SIM_LAG" CASTER_SIM_JITTER_MS="$SIM_JITTER"
                  CASTER_SIM_LOSS_PCT="$SIM_LOSS")
    [ -n "$iseed" ] && envargs+=("CASTER_SIM_SEED=$iseed")
    if [ "$SIM_ACTIVE" = "1" ]; then
        # InGame is required for the sim to exercise rollback (CharaSelect
        # idles with no meaningful PlayerInputs). Auto-input drives the
        # match from chara-select into a round; the 'diverge' pattern keeps
        # both peers moving apart (no damage → no KO → sustained InGame).
        envargs+=(CASTER_AUTO_INPUT=1 CASTER_AUTO_INPUT_PATTERN=diverge)
    fi
    ( cd "$GAME_DIR" && env "${envargs[@]}" wine caster.exe "$@" ) >"$logfile" 2>&1 &
}

# Per-instance sim seeds → uncorrelated (realistic) loss patterns on each
# side. Empty SIM_SEED → leave unset so each DLL uses its own random_device.
if [ -n "$SIM_SEED" ]; then
    SIM_HOST_SEED="$SIM_SEED"; SIM_JOIN_SEED=$((SIM_SEED + 7))
else
    SIM_HOST_SEED=""; SIM_JOIN_SEED=""
fi

# --- host --------------------------------------------------------------------
log "starting HOST: port=$PORT rollback=$ROLLBACK delay=$DELAY"
run_caster "$HOST_OUT" "$SIM_HOST_SEED" --host --port="$PORT" \
    "--rollback=$ROLLBACK" "--delay=$DELAY" "--name=P1-Host"

port_up=0
for _ in $(seq 1 40); do                       # up to 20 s
    if command -v ss >/dev/null \
       && ss -ulnH "sport = :$PORT" 2>/dev/null | grep -q .; then
        port_up=1; break
    fi
    sleep 0.5
done
if [ "$port_up" = "1" ]; then
    log "host listening on UDP $PORT"
else
    fail "host never bound UDP $PORT (see $HOST_OUT)"
fi
sleep 2                                        # let the host session settle

# --- joiner (with one retry against startup races) ---------------------------
start_join() {
    run_caster "$JOIN_OUT" "$SIM_JOIN_SEED" "--join=127.0.0.1:$PORT" \
        "--rollback=$ROLLBACK" "--delay=$DELAY" "--name=P2-Join"
}

booted=0
for attempt in 1 2; do
    log "starting JOINER (attempt $attempt): 127.0.0.1:$PORT rollback=$ROLLBACK delay=$DELAY"
    start_join
    for i in $(seq 1 50); do                   # up to 50 s for both games to boot
        if [ "$(mbaa_count)" -ge 2 ]; then booted=1; break; fi
        if [ $((i % 5)) = 0 ]; then
            log "  t+${i}s: wine exe processes=$(procs) mbaa=$(mbaa_count)"
        fi
        sleep 1
    done
    [ "$booted" = "1" ] && break
    if [ "$attempt" = "1" ]; then
        log "games did not boot — retrying joiner once"
        echo "--- joiner stdout tail ---"; tail -5 "$JOIN_OUT" || true
        echo "--- host  stdout tail ---"; tail -5 "$HOST_OUT" || true
        sleep 3
    fi
done
if [ "$booted" != "1" ]; then
    echo "=== POST-MORTEM: joiner stdout (last 20) ==="; tail -20 "$JOIN_OUT" || true
    echo "=== POST-MORTEM: host stdout (last 20) ==="; tail -20 "$HOST_OUT" || true
    echo "=== POST-MORTEM: DLL logs (last 5 each) ==="
    tail -5 "${LOG_DIR}/host_debug.log" 2>/dev/null || true
    tail -5 "${LOG_DIR}/join_debug.log" 2>/dev/null || true
    fail "both MBAA.exe never appeared"
fi
log "both games are up (MBAA.exe x$(mbaa_count))"

# --- monitor -----------------------------------------------------------------
log "monitoring for ${DURATION}s ..."
elapsed=0
while [ "$elapsed" -lt "$DURATION" ]; do
    if [ "$(mbaa_count)" -lt 2 ]; then
        fail "an MBAA.exe died mid-match at t=${elapsed}s"
    fi
    if ! pgrep -f "$(basename "$GAME_DIR")[\\\\/]caster\.exe.*--host" >/dev/null 2>&1; then
        fail "HOST launcher exited early at t=${elapsed}s"
    fi
    sleep 1; elapsed=$((elapsed + 1))
done
log "match stable for the full ${DURATION}s"

# --- report ------------------------------------------------------------------
host_new_desync=$(( $(count_matches "${LOG_DIR}/host_debug.log" "$desync_pat") - host_desync_before ))
join_new_desync=$(( $(count_matches "${LOG_DIR}/join_debug.log" "$desync_pat") - join_desync_before ))

echo
echo "==================== RESULT ===================="
echo "duration monitored                  : ${DURATION}s"
if [ "$SIM_ACTIVE" = "1" ]; then
    echo "network sim (both instances)      : lag=${SIM_LAG}ms jitter=${SIM_JITTER}ms loss=${SIM_LOSS}% (seeds ${SIM_HOST_SEED:-rand}/${SIM_JOIN_SEED:-rand})"
fi
echo "MBAA.exe alive at end               : $(mbaa_count)/2"
echo "new desync/mismatch lines (host)    : ${host_new_desync}"
echo "new desync/mismatch lines (joiner)  : ${join_new_desync}"

last_event() { # <file>
    [ -f "$1" ] && grep -E '\[session start\]|state-transition' "$1" | tail -2 || true
}
echo "--- last DLL state events (host) ---";  last_event "${LOG_DIR}/host_debug.log"
echo "--- last DLL state events (joiner) ---"; last_event "${LOG_DIR}/join_debug.log"
echo "================================================"
echo

if [ "$host_new_desync" -eq 0 ] && [ "$join_new_desync" -eq 0 ]; then
    log "PASS — stable session, no desync signatures"
    exit 0
else
    log "ATTENTION — desync signatures found; inspect ${LOG_DIR}/{host,join}_debug.log"
    exit 1
fi
