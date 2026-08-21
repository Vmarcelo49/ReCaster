#!/usr/bin/env bash
# watch-logs — live, color-coded view of the DLL netplay debug logs.
#
# Tails caster/host_debug.log and caster/join_debug.log side by side,
# filtered to the interesting events, so you can keep an eye on a match
# while testing (rollback storms, spin-block stalls, desyncs, disconnects).
#
# Usage:
#   ./scripts/watch-logs.sh            # filtered view of both logs
#   ./scripts/watch-logs.sh --all      # unfiltered
#   ./scripts/watch-logs.sh -n 200     # backfill last 200 lines per log (default 40)
#   ./scripts/watch-logs.sh --host     # only the host log (--join likewise)
#
# Env:
#   RECASTER_GAME_DIR   game folder (default: <repo>/MBAACC)

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
LOG_DIR="${RECASTER_GAME_DIR:-${ROOT_DIR}/MBAACC}/caster"

FILTER=1; BACKFILL=40; WHICH=both
while [ $# -gt 0 ]; do
    case "$1" in
        --all)  FILTER=0; shift ;;
        -n)     BACKFILL="$2"; shift 2 ;;
        --host) WHICH=host; shift ;;
        --join) WHICH=join; shift ;;
        *)      echo "usage: $0 [--all] [-n LINES] [--host|--join]" >&2; exit 2 ;;
    esac
done

R=$'\e[31m'; Y=$'\e[33m'; G=$'\e[32m'; D=$'\e[2m'; NC=$'\e[0m'

INTERESTING='EVENT|\[session|^F [0-9]+.*rb:[^n]|desync|mismatch|disconnect|error|critical|timeout'

colorize() {
    sed -u \
        -e "s/[Dd]esync|mismatch|disconnect|[Ee]rror|critical|timeout/${R}&${NC}/g" \
        -e "s/rollback|spin-(block|stall)/${Y}&${NC}/g" \
        -e "s/\[session start\]|state-transition|connected/${G}&${NC}/g"
}

tag() { # <label> <color>
    sed -u "s/^/${2}[${1}]${NC} /"
}

pids=()
start_stream() { # <file> <label> <color>
    local file="$1" label="$2" color="$3"
    if [ ! -f "$file" ]; then
        echo "${D}-- waiting for ${file} --${NC}"
    fi
    if [ "$FILTER" = "1" ]; then
        ( tail -n "$BACKFILL" -F "$file" 2>/dev/null \
          | grep --line-buffered -iE "$INTERESTING" \
          | colorize | tag "$label" "$color" ) &
    else
        ( tail -n "$BACKFILL" -F "$file" 2>/dev/null \
          | colorize | tag "$label" "$color" ) &
    fi
    pids+=($!)
}

echo "${D}watch-logs — Ctrl+C to stop (filter=$( [ "$FILTER" = 1 ] && echo on || echo off ), backfill=${BACKFILL})${NC}"

if [ "$WHICH" != "join" ]; then start_stream "${LOG_DIR}/host_debug.log" HOST "$Y"; fi
if [ "$WHICH" != "host" ]; then start_stream "${LOG_DIR}/join_debug.log" JOIN "$G"; fi

trap 'kill ${pids[*]} 2>/dev/null; exit 0' INT TERM EXIT
wait
