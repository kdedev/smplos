#!/bin/bash
# The full updater now upgrades Hyprland together with all system libraries.
# Retain this migration name so fleet markers remain compatible.
set -euo pipefail

if [[ "${SMPLOS_SYSTEM_UPGRADED:-0}" != 1 ]]; then
    echo "  Hyprland catch-up requires a completed full Update OS."
    echo "  No independent or partially pinned package transaction was attempted."
    exit 75
fi
echo "  Hyprland was handled by the full system transaction."
