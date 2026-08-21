# scripts/vdisplay.sh — shared helpers for a hidden virtual display.
#
# Uses kwin_wayland's built-in virtual framebuffer backend (KDE ships it),
# so no Xvfb / sudo is needed. Wine talks to it through its Wayland driver
# (launch wine with DISPLAY unset + WAYLAND_DISPLAY=<socket>).
#
# Source this file:
#   source "$(dirname "${BASH_SOURCE[0]}")/vdisplay.sh"
#
# Functions:
#   vdisp_running <name>          -> 0 if the virtual display is up
#   vdisp_ensure <name> <W> <H>   -> start it if needed, wait for socket
#   vdisp_stop <name>             -> shut it down and clean the socket

VDISP_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"

_vdisp_socket() { printf '%s/%s' "$VDISP_RUNTIME_DIR" "$1"; }

vdisp_running() {
    local name="$1"
    pgrep -f "kwin_wayland --virtual.*--socket ${name}\$" >/dev/null 2>&1 \
        || pgrep -f "kwin_wayland --virtual.*--socket ${name} " >/dev/null 2>&1
}

# PID of the dbus-run-session wrapper when vdisp_ensure started it this
# process; 0 when the display was already running (caller must not stop it).
VDISP_STARTED_PID=0

vdisp_ensure() { # <name> <width> <height>
    local name="$1" w="$2" h="$3"

    if vdisp_running "$name"; then
        echo "[vdisplay] reusing running virtual display '${name}'"
        return 0
    fi

    # Stale socket guard (e.g. after a hard reboot).
    rm -f "$(_vdisp_socket "$name")" "$(_vdisp_socket "$name").lock"

    echo "[vdisplay] starting virtual display '${name}' (${w}x${h}, headless)"
    VDISP_STARTED_PID=0
    dbus-run-session -- \
        kwin_wayland --virtual --socket "$name" --width "$w" --height "$h" \
        >/dev/null 2>&1 &
    VDISP_STARTED_PID=$!

    local i
    for i in $(seq 1 50); do               # up to 10 s
        if [ -S "$(_vdisp_socket "$name")" ]; then
            sleep 1                         # let kwin finish wiring clients
            return 0
        fi
        sleep 0.2
    done

    echo "[vdisplay] ERROR: socket '${name}' never appeared" >&2
    return 1
}

vdisp_stop() { # <name>
    local name="$1" pids
    pids=$(pgrep -f "dbus-run-session -- kwin_wayland --virtual --socket ${name}\$" 2>/dev/null || true)
    if [ -n "$pids" ]; then kill $pids 2>/dev/null || true; fi
    pids=$(pgrep -f "kwin_wayland --virtual --socket ${name}" 2>/dev/null || true)
    if [ -n "$pids" ]; then kill $pids 2>/dev/null || true; fi
    rm -f "$(_vdisp_socket "$name")" "$(_vdisp_socket "$name").lock"
}
