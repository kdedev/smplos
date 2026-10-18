# shellcheck shell=bash
smplos_reboot_pending() {
    local marker="${1:-$HOME/.local/state/smplos/reboot-pending}"
    local boot_file="${2:-/proc/sys/kernel/random/boot_id}" saved current
    if [[ $# -eq 0 && ! -e "$marker" ]]; then
        marker="$HOME/.local/state/smplos/update/reboot-pending"
    fi
    [[ -e "$marker" || -L "$marker" ]] || return 1
    if ! saved=$(tr -d '-' < "$marker") ||
        ! current=$(tr -d '-' < "$boot_file"); then
        echo "smplOS: cannot read the pending-reboot marker or current boot ID" >&2
        return 2
    fi
    if [[ ! "$saved" =~ ^[0-9a-f]{32}$ || ! "$current" =~ ^[0-9a-f]{32}$ ]]; then
        echo "smplOS: invalid pending-reboot marker or current boot ID" >&2
        return 2
    fi
    [[ "$saved" == "$current" ]]
}
