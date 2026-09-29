#!/bin/bash
# Make the stock Nemo shortcut workspace-local without replacing user bindings.
set -euo pipefail

command -v focus-or-launch-workspace >/dev/null || {
  echo "  Workspace-local launcher is missing; update scripts before retrying" >&2
  exit 1
}

update_binding() {
  local conf="$1" pattern="$2" replacement="$3" backup
  [[ -f "$conf" ]] || return 0
  grep -qE "$pattern" "$conf" || return 0
  backup=$(mktemp "$conf.pre-workspace-nemo.XXXXXX")
  cp -p "$conf" "$backup"
  cmp "$conf" "$backup"
  sed -i -E "s|$pattern|$replacement|" "$conf"
  echo "  Updated Nemo shortcut in $conf (backup: $backup)"
}

hypr_pattern='^(bindd[[:space:]]*=[[:space:]]*SUPER[[:space:]]+SHIFT[[:space:]]*,[[:space:]]*[Ff][[:space:]]*,[^,]*,[[:space:]]*exec[[:space:]]*,[[:space:]]*)focus-or-launch[[:space:]]+nemo[[:space:]]+nemo([[:space:]]*(#.*)?)$'
for conf in "$HOME/.config/smplos/bindings.conf" "$HOME/.config/hypr/bindings.conf"; do
  update_binding "$conf" "$hypr_pattern" '\1focus-or-launch --current-workspace nemo nemo\2'
done

# Patch both the assembled niri config and its input, so later theme changes
# cannot assemble the old binding back in. niri watches config.kdl for reloads.
niri_pattern='^([[:space:]]*Mod\+Shift\+[fF][[:space:]].*"sh"[[:space:]]+"-c"[[:space:]]+")focus-or-launch nemo nemo(".*)$'
for conf in \
  "${SMPLOS_PATH:-$HOME/.local/share/smplos}/configs/niri/binds.kdl" \
  "$HOME/.config/niri/binds.kdl" \
  "$HOME/.config/niri/config.kdl"; do
  update_binding "$conf" "$niri_pattern" '\1focus-or-launch --current-workspace nemo nemo\2'
done

source "$(dirname "${BASH_SOURCE[0]}")/../src/shared/lib/smplos-session-env.sh"
if smplos_have_hyprland; then
  smplos_run_as_user hyprctl reload
fi
