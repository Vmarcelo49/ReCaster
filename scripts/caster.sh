#!/usr/bin/env bash
# caster — wrapper script that suppresses Wine's noisy fixme messages
# so --help and error messages are readable.
#
# Only the fixme channel is suppressed — err and warn stay visible so
# real problems aren't hidden.
#
# Usage:
#   ./caster.sh [args...]    # same args as caster.exe
#
# The user can override by setting WINEDEBUG themselves:
#   WINEDEBUG=+module ./caster.sh   # enables debug for a specific module

if [ -z "$WINEDEBUG" ]; then
    export WINEDEBUG=fixme-all
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
exec wine "$SCRIPT_DIR/caster.exe" "$@"
