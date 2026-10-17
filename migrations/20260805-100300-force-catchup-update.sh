#!/bin/bash
# Older fleet catch-up performed a second upgrade from inside migrations.
# Package upgrades and bundle receipts now belong to the full updater.
set -euo pipefail

if [[ "${SMPLOS_SYSTEM_UPGRADED:-0}" != 1 ]]; then
    echo "  Fleet package catch-up requires a completed full Update OS."
    echo "  Retry with the refreshed smplos-update entrypoint."
    exit 75
fi
echo "  Fleet packages were handled by the full system transaction."
