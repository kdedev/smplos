#!/bin/bash
# Migration: Record GPU faults and keep the journal close to current on disk.
#
# Context: NVIDIA display hangs (Xid 56, flip timeouts) froze the monitors and
#          ended in a reboot that erased Hyprland's tmpfs log and the live DRM
#          state, while journald only synced to disk every 5 minutes.
#          smplos-gpu-watch.service (scripts are synced before migrations)
#          snapshots that state to /var/log/smplos/gpu-incidents on a fault.
# Safety:  System-level and idempotent; journald restarts only when its
#          configuration changed. The user session is not touched.
set -uo pipefail

SYSTEM="$(dirname "${BASH_SOURCE[0]}")/../src/shared/system"
UNIT=smplos-gpu-watch.service
DROPIN=/etc/systemd/journald.conf.d/60-smplos-sync.conf
status=0

if ! command -v smplos-gpu-watch >/dev/null || ! command -v smplos-gpu-snapshot >/dev/null; then
    echo "  GPU fault recorder scripts are not installed - update OS scripts first"
    exit 1
fi

if sudo cmp -s "$SYSTEM/journald/${DROPIN##*/}" "$DROPIN"; then
    echo "  journal sync interval already current"
elif sudo install -Dm644 "$SYSTEM/journald/${DROPIN##*/}" "$DROPIN" \
        && sudo systemctl restart systemd-journald; then
    echo "  Journal now syncs to disk every 15s"
else
    echo "  WARNING: could not apply $DROPIN"
    status=1
fi

if ! sudo cmp -s "$SYSTEM/systemd/$UNIT" "/etc/systemd/system/$UNIT"; then
    sudo install -Dm644 "$SYSTEM/systemd/$UNIT" "/etc/systemd/system/$UNIT" \
        && sudo systemctl daemon-reload || { echo "  WARNING: could not install $UNIT"; exit 1; }
fi
if sudo systemctl enable "$UNIT" && sudo systemctl restart "$UNIT"; then
    echo "  GPU fault recorder enabled (snapshots in /var/log/smplos/gpu-incidents)"
else
    echo "  WARNING: could not enable $UNIT"
    status=1
fi
exit "$status"
