#!/bin/bash
# Retire Logseq installations, never their graphs, preferences or Flatpak data.
set -euo pipefail

die() {
    echo "  ERROR: $*; will retry on next update" >&2
    exit 1
}

backup_file() {
    local source="$1" backup_root backup
    backup_root="${XDG_STATE_HOME:-$HOME/.local/state}/smplos/retired-apps"
    mkdir -p "$backup_root" || die "Cannot create retirement backup directory"
    backup=$(mktemp -d "$backup_root/logseq.XXXXXX") || die "Cannot stage retirement backup"
    if [[ -r "$source" ]]; then
        cp -p -- "$source" "$backup/${source##*/}" &&
            cmp -- "$source" "$backup/${source##*/}" || die "Cannot verify backup for $source"
    else
        sudo cat -- "$source" > "$backup/${source##*/}" &&
            chmod --reference="$source" "$backup/${source##*/}" &&
            sudo cmp -- "$source" "$backup/${source##*/}" || die "Cannot verify private-file backup for $source"
    fi
    printf '%s\n' "$source" > "$backup/path" || die "Cannot record backup path"
    printf '%s\n' "$backup/${source##*/}"
}

retire_launcher() {
    local desktop="$1" pattern="$2" backup
    if [[ -f "$desktop" && ! -L "$desktop" ]] && grep -qE "$pattern" "$desktop"; then
        backup=$(backup_file "$desktop")
        rm -- "$desktop" || die "Cannot retire launcher $desktop"
        echo "  Retired Logseq launcher; verified backup: $backup"
    fi
}

installed=$(pacman -Qq) || die "Cannot inventory packages"
remove=()
while IFS= read -r package; do
    case "${package%-debug}" in
        logseq|logseq-bin|logseq-git|logseq-desktop|logseq-desktop-bin|logseq-desktop-git)
            remove+=("$package") ;;
    esac
done <<< "$installed"
if (( ${#remove[@]} )); then
    echo "  Replacing Logseq with Grafium; personal graphs and settings are untouched"
    sudo pacman -R --noconfirm -- "${remove[@]}" || die "Logseq package removal failed"
fi

first_boot_list=/opt/flatpaks/install-online.txt
if [[ -e "$first_boot_list" || -L "$first_boot_list" ]]; then
    [[ -f "$first_boot_list" ]] || die "Cannot read $first_boot_list"
    entries=$(sudo cat -- "$first_boot_list") || die "Cannot read $first_boot_list"
    if grep -qxF com.logseq.Logseq <<< "$entries"; then
        [[ ! -L "$first_boot_list" ]] || die "Refusing to replace custom symlink $first_boot_list"
        backup=$(backup_file "$first_boot_list")
        root_stage=""
        trap 'if [[ -n "$root_stage" ]]; then
            sudo rm -f -- "$root_stage" || echo "  ERROR: Cannot clean staging file $root_stage" >&2
        fi' EXIT
        root_stage=$(sudo mktemp "$first_boot_list.XXXXXX") || die "Cannot stage first-boot list"
        awk '$0 != "com.logseq.Logseq"' "$backup" |
            sudo tee "$root_stage" >/dev/null || die "Cannot write first-boot list"
        sudo chmod --reference="$first_boot_list" "$root_stage" || die "Cannot preserve first-boot list mode"
        sudo cmp -- "$first_boot_list" "$backup" || die "First-boot list changed during retirement"
        sudo mv -fT -- "$root_stage" "$first_boot_list" || die "Cannot publish first-boot list"
        root_stage=""
        echo "  Removed first-boot Logseq reinstall entry; verified backup: $backup"
    fi
fi

if command -v flatpak >/dev/null 2>&1; then
    for scope in user system; do
        installed=$(flatpak list "--$scope" --app --columns=application) || die "Cannot inventory $scope Flatpaks"
        if grep -qxF com.logseq.Logseq <<< "$installed"; then
            command=(flatpak uninstall "--$scope" --noninteractive --assumeyes com.logseq.Logseq)
            [[ "$scope" != system ]] || command=(sudo "${command[@]}")
            "${command[@]}" || die "Logseq $scope Flatpak removal failed"
        fi
    done
fi

retire_launcher "$HOME/.local/share/applications/Logseq-appimage.desktop" \
    '^Exec=/opt/appimages/Logseq\.AppImage([[:space:]]|$)'
retire_launcher "$HOME/.local/share/applications/com.logseq.Logseq.desktop" \
    '^(X-Flatpak=com\.logseq\.Logseq|Exec=.*flatpak.*[[:space:]]com\.logseq\.Logseq([[:space:]].*)?)$'
if [[ -f /opt/appimages/Logseq.AppImage ]]; then
    sudo rm -- /opt/appimages/Logseq.AppImage
fi
echo "  Logseq retired; Grafium is the bundled notes app. Existing Logseq data is preserved."
