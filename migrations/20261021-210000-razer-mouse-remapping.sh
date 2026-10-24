#!/bin/bash
# Settings > Mouse: device-scoped Razer Naga V2 HyperSpeed remapping.
# Deliver the compatible control-surface package before enabling the backend.
# Never change saved mouse/keypad mappings or restart a running remapper.
set -euo pipefail

SMPLOS_PATH="${SMPLOS_PATH:-$HOME/.local/share/smplos}"
REPO="${SMPLOS_REPO:-$SMPLOS_PATH/repo}"
UDEV_DIR="${SMPLOS_UDEV_RULES_DIR:-/etc/udev/rules.d}"
SYSFS="${SMPLOS_MOUSE_SYSFS:-/sys/bus/usb/devices}"
UINPUT_SYSFS="${SMPLOS_MOUSE_UINPUT_SYSFS:-/sys/devices/virtual/misc/uinput}"
UNIT_DIR="${XDG_CONFIG_HOME:-$HOME/.config}/systemd/user"
UNIT_STATE="${XDG_STATE_HOME:-$HOME/.local/state}/smplos/mouse-units"
source "$REPO/src/shared/lib/smplos-keypad-units.sh"
source "$REPO/src/shared/lib/smplos-session-env.sh"

if ! smplos_mouse_check_runtime "${SMPLOS_MOUSE_DAEMON:-/usr/bin/mouse-remapperd}"; then
    echo "  Mouse remapping deferred: a compatible mouse-remapperd release is required. Existing units and mappings retained; retry Update OS after delivery." >&2
    exit 75
fi

rule=70-smplos-mice.rules
src="$REPO/src/shared/system/udev/$rule"
dst="$UDEV_DIR/$rule"
[[ -f "$src" ]] || { echo "  ERROR: $rule missing from repo" >&2; exit 1; }

# Reconcile before triggering hotplug so the service exists when udev asks for it.
smplos_mouse_sync_units "$REPO/src/shared/configs/systemd/user" "$UNIT_DIR" "$UNIT_STATE"
if [[ $SMPLOS_MOUSE_UNITS_CHANGED -eq 1 ]] && smplos_have_user_bus; then
    smplos_run_as_user systemctl --user daemon-reload
fi

if ! cmp -s "$src" "$dst"; then
    if [[ -e "$dst" || -L "$dst" ]]; then
        backup=$(sudo mktemp -d "$UDEV_DIR/smplos-mouse-backup.XXXXXX")
        sudo cp -a "$dst" "$backup/$rule"
        sudo cmp -s "$dst" "$backup/$rule"
    fi
    sudo install -Dm644 "$src" "$dst"
    sudo udevadm control --reload-rules
    for dev in "$SYSFS"/*; do
        [[ "$(cat "$dev/idVendor" 2>/dev/null)" == 1532 \
            && "$(cat "$dev/idProduct" 2>/dev/null)" == 00b4 ]] || continue
        sudo udevadm trigger --action=change --parent-match="$(readlink -f "$dev")"
    done
    if [[ -e "$UINPUT_SYSFS" ]]; then
        sudo udevadm trigger --action=change "$UINPUT_SYSFS"
    fi
    echo "  Installed Razer mouse access and device-triggered service rules"
fi
echo "  Mouse remapping integration configured (custom units and mappings preserved)"
