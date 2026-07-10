#!/usr/bin/env bash
# caster — wrapper script that suppresses Wine's verbose debug output
# (radv warnings, xinput fixme, etc.) so --help and error messages are
# readable.
#
# The caster.exe binary itself also sets WINEDEBUG=-all internally via
# putenv() at the very start of main(), but some Wine warnings (like
# the radv Vulkan warning) are printed BEFORE main() runs, during DLL
# loading. This wrapper sets WINEDEBUG in the environment BEFORE wine
# starts, so even those early warnings are suppressed.
#
# Usage:
#   ./caster.sh [args...]    # same args as caster.exe
#
# The user can override this by setting WINEDEBUG themselves:
#   WINEDEBUG=+module ./caster.sh   # enables debug for a specific module

# Only set WINEDEBUG if the user hasn't already configured it.
if [ -z "$WINEDEBUG" ]; then
    export WINEDEBUG=-all
fi

# Find caster.exe next to this script.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
exec wine "$SCRIPT_DIR/caster.exe" "$@"
