#!/bin/bash
# SMPLOS_UPDATE_PRIORITY=optional
# Migration: install hyprpicker on existing installs (the Super+Print color picker).
#
# Context: the stock Super+Print binding runs `hyprpicker -a`, but the package was
#          never in the Hyprland package list, so machines installed from an
#          older ISO have a color-picker shortcut that does nothing. Update OS
#          only upgrades packages that are already installed, so a package that
#          existing machines lack has to arrive through a migration.
#
# Safety:  Idempotent: exits 0 when hyprpicker is installed. The install waits
#          (exit 75, retried on the next Update OS) until the full system
#          upgrade has completed, so it can never pull new packages against a
#          stale sync database and leave a partial upgrade behind. A failed
#          install stays unmarked and retries; as an optional migration it never
#          blocks the core update.
set -uo pipefail

if pacman -Q hyprpicker &>/dev/null; then
    echo "  hyprpicker already installed - nothing to do"
    exit 0
fi

if [[ "${SMPLOS_SYSTEM_UPGRADED:-0}" != 1 ]]; then
    echo "  hyprpicker install deferred until the complete system upgrade has run"
    exit 75
fi

if ! sudo pacman -S --needed --noconfirm hyprpicker; then
    echo "  ERROR: could not install hyprpicker - will retry on next update"
    exit 1
fi

if ! pacman -Q hyprpicker &>/dev/null; then
    echo "  ERROR: hyprpicker is still not installed - will retry on next update"
    exit 1
fi
echo "  Installed $(pacman -Q hyprpicker) - the Super+Print color picker now works"
