#!/usr/bin/env bash
# spectest — full spectator-mode integration test on localhost.
#
# Brings up THREE instances on the hidden virtual display:
#   1. HOST    (--host,  auto-mash enabled, pattern configurable)
#   2. JOINER  (--join,  auto-mash enabled)
#   3. SPECTATOR (--spec, attached a few seconds into the match)
#
# Pass criteria:
#   - three MBAA.exe processes survive the whole monitoring window
#   - host DLL log shows the spectator being promoted
#     ('spectator promoted' from SpectatorManager)
#   - spectator DLL log shows it received the stream
#     ('spectate_client: SpectateConfig received')
#
# Usage:
#   ./scripts/spectest.sh [options]
#     --duration=S      seconds to monitor after the spectator joins (def: 40)
#     --rollback=N      rollback window frames                   (def: 4)
#     --delay=N         input delay frames                       (def: 1)
#     --port=P          host UDP port                            (def: 46318)
#     --pattern=NAME    auto-mash pattern: diverge|collide|idle|random (def: random)
#                        NOTE: 'collide' walks forward + mashes A, which
#                        rarely deals damage or ends rounds — use 'random'
#                        to make the match actually progress.
#     --relay-room=#ABCD  spectate via relay room instead of direct IP
#     --skip-build      don't run deploy.sh first
#     --keep-display    leave the virtual display running afterwards
#     --visible         run on the REAL desktop (DISPLAY=:0) instead of the
#                       hidden virtual display, and leave all instances
#                       RUNNING after the report so a human can watch them.
#                       Stop them later with:
#                         pkill -f 'MBAACC[\\/]caster\.exe'
#
# Exit code: 0 = PASS, 1 = FAIL.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
GAME_DIR="${RECASTER_GAME_DIR:-${ROOT_DIR}/MBAACC}"
LOG_DIR="${GAME_DIR}/caster"
DLL_LOG="${GAME_DIR}/debug.log"          # common::logger output of both DLLs

DURATION=40; ROLLBACK=4; DELAY=1; PORT=46318
PATTERN="random"; ROOM=""
SKIP_BUILD=0; KEEP_DISPLAY=0; VISIBLE=0
while [ $# -gt 0 ]; do
    arg="$1"
    case "$arg" in
        --skip-build)   SKIP_BUILD=1; shift; continue ;;
        --keep-display) KEEP_DISPLAY=1; shift; continue ;;
        --visible)      VISIBLE=1; shift; continue ;;
        --duration|--duration=*|--rollback|--rollback=*|--delay|--delay=*|--port|--port=*)
            if [[ "$arg" == *=* ]]; then val="${arg#*=}"; shift
            else val="${2:-}"; shift 2; fi
            case "${arg%%=*}" in
                --duration) DURATION="$val" ;;
                --rollback) ROLLBACK="$val" ;;
                --delay)    DELAY="$val" ;;
                --port)     PORT="$val" ;;
            esac ;;
        --pattern|--pattern=*)
            if [[ "$arg" == *=* ]]; then PATTERN="${arg#*=}"; shift
            else PATTERN="${2:-}"; shift 2; fi ;;
        --relay-room|--relay-room=*)
            if [[ "$arg" == *=* ]]; then ROOM="${arg#*=}"; shift
            else ROOM="${2:-}"; shift 2; fi ;;
        *) echo "spectest: unknown option '$arg'" >&2; exit 2 ;;
    esac
done

source "${SCRIPT_DIR}/vdisplay.sh"
DISPLAY_NAME="${VRUN_DISPLAY:-recaster-spectest}"
WIDTH=1280; HEIGHT=800

HOST_OUT="$(mktemp /tmp/recaster-spectest-host.XXXX.log)"
JOIN_OUT="$(mktemp /tmp/recaster-spectest-join.XXXX.log)"
SPEC_OUT="$(mktemp /tmp/recaster-spectest-spec.XXXX.log)"

log()  { echo "[spectest] $*"; }
fail() { echo "[spectest] FAIL: $*" >&2; exit 1; }

EXES="$(basename "$GAME_DIR")[\\\\/](caster|MBAA)\\.exe"

# NOTE: pgrep exits 1 when nothing matches; under `set -e` a failing
# command substitution would kill the script silently, so every counter
# tolerates a failed pgrep.
procs()      { local n=0; n=$(pgrep -f "$EXES" 2>/dev/null | wc -l) || true; echo "$n"; }
mbaa_count() { local n=0; n=$(pgrep -f "$(basename "$GAME_DIR")[\\\\/]MBAA\\.exe" 2>/dev/null | wc -l) || true; echo "$n"; }

count_matches() {
    local f="$1" pat="$2" out=""
    if [ -f "$f" ]; then out=$(grep -ciE "$pat" "$f" 2>/dev/null) || true; fi
    printf '%s' "${out:-0}"
}

CONFIG_INI="${LOG_DIR}/config.ini"
CONFIG_BAK=""
override_config() {
    [ -f "$CONFIG_INI" ] || return 0
    CONFIG_BAK="${CONFIG_INI}.nettest-bak"
    cp "$CONFIG_INI" "$CONFIG_BAK"
    if grep -q '^relays=' "$CONFIG_INI"; then
        sed -i 's/^relays=.*/relays=#/' "$CONFIG_INI"
    else
        printf '\nrelays=#\n' >> "$CONFIG_INI"
    fi
}
restore_config() {
    if [ -n "$CONFIG_BAK" ] && [ -f "$CONFIG_BAK" ]; then
        mv -f "$CONFIG_BAK" "$CONFIG_INI"; CONFIG_BAK=""
    fi
}
override_config

cleanup() {
    log "cleaning up..."
    restore_config
    if [ "$VISIBLE" = "1" ]; then
        # Visible mode: leave the three instances running so a human can
        # inspect them. Report how to stop them.
        log "VISIBLE mode — instances left RUNNING (MBAA=$(mbaa_count))."
        log "stop later with:  pkill -f 'MBAACC[\\\\/](caster|MBAA)\\.exe'"
        return 0
    fi
    pkill -f "$EXES" 2>/dev/null || true
    sleep 3
    if [ "$(procs)" -gt 0 ]; then
        wineserver -k 2>/dev/null || true; sleep 1
    fi
    if [ "$KEEP_DISPLAY" != "1" ]; then vdisp_stop "$DISPLAY_NAME"; fi
    # stdout logs kept in /tmp for post-mortem (tiny text files)
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

if [ "$SKIP_BUILD" != "1" ]; then "$SCRIPT_DIR/deploy.sh" quick; fi
if [ "$VISIBLE" != "1" ]; then
    vdisp_ensure "$DISPLAY_NAME" "$WIDTH" "$HEIGHT" || fail "virtual display failed to start"
fi

run_caster() { # <logfile> <env-extra...> -- <caster args...>
    local logfile="$1"; shift
    local envs=()
    while [ "$1" != "--" ]; do envs+=("$1"); shift; done
    shift
    # The spectator also gets auto-input: it must drive its own menu
    # confirms (RetryMenu forced-rematch etc.). InGame local input is
    # already gated off for spectators in the DLL, so the replay stream
    # stays authoritative.
    envs+=(CASTER_AUTO_INPUT=1)

    if [ "$VISIBLE" = "1" ]; then
        ( cd "$GAME_DIR" \
          && env DISPLAY="${REAL_DISPLAY:-:0}" WINEDEBUG=fixme-all \
               "${envs[@]}" wine caster.exe "$@" ) >"$logfile" 2>&1 &
    else
        ( cd "$GAME_DIR" \
          && env -u DISPLAY WAYLAND_DISPLAY="$DISPLAY_NAME" WINEDEBUG=fixme-all \
               "${envs[@]}" wine caster.exe "$@" ) >"$logfile" 2>&1 &
    fi
}

MASH_ENV=(CASTER_AUTO_INPUT=1 "CASTER_AUTO_INPUT_PATTERN=${PATTERN}")

# Optional deep diagnostics: per-batch input fingerprints (spec-tx / spec-rx)
# plus SEND/RECV PlayerInputs traces on the players.
if [ "${SPECTEST_TRACE:-0}" = "1" ]; then
    MASH_ENV+=(CASTER_LOG_REMOTE_INPUTS=1)
fi

# --- phase 1: host -----------------------------------------------------------
log "starting HOST: port=$PORT rollback=$ROLLBACK delay=$DELAY mash=${PATTERN}"
run_caster "$HOST_OUT" "${MASH_ENV[@]}" -- \
    --host --port="$PORT" "--rollback=$ROLLBACK" "--delay=$DELAY" "--name=P1-Host"

port_up=0
for _ in $(seq 1 40); do
    if ss -ulnH "sport = :$PORT" 2>/dev/null | grep -q .; then port_up=1; break; fi
    sleep 0.5
done
[ "$port_up" = "1" ] || fail "host never bound UDP $PORT (see $HOST_OUT)"
log "host listening on UDP $PORT"
sleep 2

# --- phase 2: joiner ---------------------------------------------------------
log "starting JOINER: 127.0.0.1:$PORT (auto-mash)"
run_caster "$JOIN_OUT" "${MASH_ENV[@]}" -- \
    "--join=127.0.0.1:$PORT" "--rollback=$ROLLBACK" "--delay=$DELAY" "--name=P2-Join"

booted=0
for _ in $(seq 1 60); do
    [ "$(mbaa_count)" -ge 2 ] && { booted=1; break; }
    sleep 1
done
[ "$booted" = "1" ] || fail "match never booted (see $JOIN_OUT / $HOST_OUT)"
log "match is up (MBAA.exe x2) — settling for 6 s before spectator joins"
sleep 6

# --- phase 3: spectator ------------------------------------------------------
spec_log_before=$(count_matches "$DLL_LOG" 'spectator|spectate')

log "starting SPECTATOR: --spec=127.0.0.1:$PORT"
if [ -n "$ROOM" ]; then
    run_caster "$SPEC_OUT" -- "--spec=#${ROOM}"
else
    run_caster "$SPEC_OUT" -- "--spec=127.0.0.1:$PORT"
fi

spec_booted=0
for _ in $(seq 1 60); do
    [ "$(mbaa_count)" -ge 3 ] && { spec_booted=1; break; }
    sleep 1
done
[ "$spec_booted" = "1" ] || {
    echo "=== POST-MORTEM: spec stdout ==="; tail -20 "$SPEC_OUT" || true
    echo "=== POST-MORTEM: dll log tail ==="; tail -8 "$DLL_LOG" 2>/dev/null || true
    fail "spectator game never appeared (MBAA count stuck at $(mbaa_count))"
}
log "spectator game is up (MBAA.exe x$(mbaa_count))"

# --- monitor -----------------------------------------------------------------
log "monitoring ${DURATION}s with all three instances ..."
elapsed=0
while [ "$elapsed" -lt "$DURATION" ]; do
    n=$(mbaa_count)
    if [ "$n" -lt 3 ]; then fail "an MBAA.exe exited at t=${elapsed}s (alive=$n)"; fi
    sleep 1; elapsed=$((elapsed + 1))
done
log "all three instances stable for ${DURATION}s"

# --- report ------------------------------------------------------------------
spec_new=$(count_matches "$DLL_LOG" 'spectator|spectate')
spec_promoted=$(count_matches "$DLL_LOG" 'spectator promoted|SpectateConfig received')
host_spec_conn=$(count_matches "$DLL_LOG" 'spectator CONNECTED')

echo
echo "==================== RESULT ===================="
echo "instances alive at end              : $(mbaa_count)/3"
echo "new spectator log lines (dll debug) : ${spec_new}"
echo "promotion/stream evidence           : ${spec_promoted} lines"
echo "launcher saw 'spectator CONNECTED'  : ${host_spec_conn}"

last_event() { [ -f "$1" ] && grep -E '\[session start\]|state-transition|role=' "$1" | tail -3 || true; }
echo "--- last structured events (host dll) ---"; last_event "${LOG_DIR}/host_debug.log"
echo "--- last structured events (spectator dll -> join_debug.log) ---"; last_event "${LOG_DIR}/join_debug.log"
echo "================================================"
echo

if [ "${spec_promoted:-0}" -ge 1 ]; then
    log "PASS — spectator attached and received the stream"
    exit 0
else
    log "FAIL — games ran but spectator stream evidence missing; inspect $DLL_LOG"
    exit 1
fi
