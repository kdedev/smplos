#!/bin/bash
# Retire only the unused 32-bit dependency, never replace PipeWire with JACK2.
set -uo pipefail

if [[ "${SMPLOS_SYSTEM_UPGRADED:-0}" != 1 ]]; then
    echo "  Retired lib32-mpg123 cleanup deferred until the complete base upgrade"
    exit 75
fi
source "${BASH_SOURCE[0]%/*}/../src/shared/lib/smplos-pkgbuild.sh" || {
    echo "  ERROR: retired-package cleanup helper is missing"
    exit 1
}
smplos_remove_retired_orphan lib32-mpg123
