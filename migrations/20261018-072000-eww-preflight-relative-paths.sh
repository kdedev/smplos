#!/bin/bash
# SMPLOS_UPDATE_PRIORITY=core
# The IPC migration may already be complete with the faulty revision 4 preflight.
set -uo pipefail

REPO="${SMPLOS_REPO:-${SMPLOS_PATH:-$HOME/.local/share/smplos}/repo}"
echo "  Repairing EWW preflight relative include resolution..."
exec bash "$REPO/migrations/20261018-060000-eww-ipc-deadlines.sh"
