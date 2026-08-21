#!/usr/bin/env bash
# vrun — run caster.exe inside a hidden virtual display (headless testing).
#
# The display is a kwin_wayland virtual framebuffer (no Xvfb, no sudo);
# Wine renders through its Wayland driver. Nothing ever appears on your
# real desktop.
#
# Usage:
#   ./scripts/vrun.sh [caster args...]     same args as caster.exe, e.g.:
#     ./scripts/vrun.sh --training
#     ./scripts/vrun.sh --host --rollback=4 --delay=1
#     ./scripts/vrun.sh --join=127.0.0.1:46318 --name=Tester
#   ./scripts/vrun.sh --stop               shut the virtual display down
#
# Env:
#   VRUN_DISPLAY   wayland socket name        (default: recaster-virt)
#   VRUN_SIZE      framebuffer size WxH       (default: 1280x800)
#   VRUN_KEEP=1    leave the display running after the app exits
#                  (useful when launching several instances manually)
#
# Exit code: caster.exe's exit code.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
GAME_DIR="${RECASTER_GAME_DIR:-${ROOT_DIR}/MBAACC}"

source "${SCRIPT_DIR}/vdisplay.sh"

DISPLAY_NAME="${VRUN_DISPLAY:-recaster-virt}"
VRUN_SIZE="${VRUN_SIZE:-1280x800}"
WIDTH="${VRUN_SIZE%%x*}"; HEIGHT="${VRUN_SIZE##*x}"

if [ "${1:-}" = "--stop" ]; then
    vdisp_stop "$DISPLAY_NAME"
    echo "[vrun] virtual display '${DISPLAY_NAME}' stopped"
    exit 0
fi

if [ ! -f "${GAME_DIR}/caster.exe" ]; then
    echo "vrun: ${GAME_DIR}/caster.exe not found — run ./scripts/deploy.sh first" >&2
    exit 1
fi

started_by_us=0
vdisp_ensure "$DISPLAY_NAME" "$WIDTH" "$HEIGHT" || exit 1
if [ "${VDISP_STARTED_PID:-0}" != "0" ]; then started_by_us=1; fi

cleanup() {
    if [ "$started_by_us" = "1" ] && [ "${VRUN_KEEP:-0}" != "1" ]; then
        vdisp_stop "$DISPLAY_NAME"
    fi
}
trap cleanup EXIT
trap 'exit 130' INT TERM

cd "$GAME_DIR"
echo "[vrun] wine caster.exe $* (display=${DISPLAY_NAME}, headless)"
env -u DISPLAY \
    WAYLAND_DISPLAY="$DISPLAY_NAME" \
    WINEDEBUG="${WINEDEBUG:-fixme-all}" \
    wine caster.exe "$@"
status=$?
echo "[vrun] caster.exe exited with ${status}"
exit "$status"
