# shellcheck shell=bash

# Compare package metadata, not filenames: epochs and pkgrel affect ordering.
smplos_latest_package() {
    local directory="$1" package="$2"
    local archive metadata name version comparison selected="" selected_version=""

    for archive in "$directory/$package-"*.pkg.tar.{zst,xz}; do
        [[ -f "$archive" ]] || continue
        if ! metadata=$(pacman -Qp -- "$archive"); then
            printf 'ERROR: Cannot read package metadata: %s\n' "$archive" >&2
            return 1
        fi
        read -r name version <<< "$metadata"
        [[ "$name" == "$package" ]] || continue

        if [[ -n "$selected" ]]; then
            comparison=$(vercmp "$version" "$selected_version") || return 1
            (( comparison > 0 )) || continue
        fi
        selected="$archive"
        selected_version="$version"
    done

    printf '%s\n' "$selected"
}

smplos_install_offline_packages() {
    local package_list="$1" package
    local mirror="${2:-/var/cache/smplos/mirror/offline}" repositories server
    local targets=()
    while IFS= read -r package || [[ -n "$package" ]]; do
        package="${package%%#*}"
        read -r package <<< "$package"
        [[ -n "$package" ]] || continue
        targets+=("offline/$package")
    done < "$package_list"

    [[ ${#targets[@]} -gt 0 ]] || return 0
    repositories=$(pacman-conf --repo-list) || return 1
    if [[ "$repositories" != "offline" ]]; then
        echo "ERROR: Offline installation requires only the offline pacman repository" >&2
        return 1
    fi
    server=$(pacman-conf --repo offline Server) || return 1
    if [[ "${server%/}" != "file://${mirror%/}" ]]; then
        printf 'ERROR: Offline repository must use the ISO mirror: %s\n' "$mirror" >&2
        return 1
    fi

    # Refresh even a same-timestamp sync DB, then use repo versions rather than
    # retained cache archives. The guards above prevent an online partial sync.
    if ! sudo pacman -Syy --noconfirm --needed "${targets[@]}"; then
        echo "ERROR: Failed to install custom packages from the offline repository" >&2
        return 1
    fi
}
