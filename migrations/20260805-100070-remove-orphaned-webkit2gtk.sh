#!/bin/bash
# Migration: Remove the orphaned webkit2gtk (WebKitGTK 4.0 API for GTK 3).
#
# Context: Arch no longer ships webkit2gtk. Systems that installed it from
#          the AUR as a dependency keep it after the app that needed it is
#          gone, and the libjxl 0.12 update removed libjxl.so.0.11 it links
#          against, so the leftover copy cannot even load.
# Safety:  Removes it only when it is an orphan (installed as a dependency and
#          required by nothing) and no sync repository provides it.
#
# Named to run with the other cleanup before the 20260805 catch-up migrations.
set -uo pipefail

source "${BASH_SOURCE[0]%/*}/../src/shared/lib/smplos-pkgbuild.sh" || {
    echo "  ERROR: retired-package cleanup helper is missing"
    exit 1
}
smplos_remove_retired_orphan webkit2gtk
