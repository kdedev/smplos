#!/bin/bash
# The full updater resolves official QEMU conflicts in its upgrade transaction.
# Do not remove a working QEMU installation in a separate migration.
set -euo pipefail

if [[ "${SMPLOS_SYSTEM_UPGRADED:-0}" != 1 ]]; then
    echo "  QEMU package catch-up requires a completed full Update OS."
    exit 75
fi
echo "  QEMU was handled by the full system transaction."
