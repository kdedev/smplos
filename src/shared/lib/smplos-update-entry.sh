# shellcheck shell=bash
smplos_update_entry_guard() {
    if [[ "${SMPLOS_UPDATE_COORDINATOR:-0}" == 1 || "${SMPLOS_UPDATER_REFRESHED:-0}" == 1 ]] &&
        [[ "${SMPLOS_UPDATE_LOCKED:-0}" == 1 &&
           "/proc/$$/fd/9" -ef "$HOME/.local/state/smplos/update/update.lock" ]]; then
        return 0
    fi
    local repo="$1" status
    source "$repo/src/shared/lib/smplos-session-env.sh"
    smplos_resolve_session_env || true
    if XDG_RUNTIME_DIR="${SMPLOS_SESSION_RUNTIME_DIR:-}" \
        WAYLAND_DISPLAY="${SMPLOS_SESSION_WAYLAND_DISPLAY:-}" \
        DISPLAY="${SMPLOS_SESSION_DISPLAY:-}" \
        XAUTHORITY="${SMPLOS_SESSION_XAUTHORITY:-}" \
        DBUS_SESSION_BUS_ADDRESS="${SMPLOS_SESSION_DBUS:-}" \
        HYPRLAND_INSTANCE_SIGNATURE="${SMPLOS_SESSION_HIS:-}" \
        python3 "$repo/src/shared/bin/smplos-update-handoff" \
            --repo "$repo" --parent "$PPID" --uid "${SMPLOS_SESSION_UID:-0}"; then
        return 0
    else
        status=$?
    fi
    [[ $status -ne 75 ]] || exit 0
    exit 1
}
