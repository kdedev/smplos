#!/bin/bash
# SMPLOS_UPDATE_PRIORITY=core
# Preserve the working package on build failure. Post-deploy activates the
# replacement daemon once, after config and palette publication.
set -uo pipefail

REPO="${SMPLOS_REPO:-${SMPLOS_PATH:-$HOME/.local/share/smplos}/repo}"
DIR="$REPO/src/shared/pkgbuilds/eww-smplos"
source "$REPO/src/shared/lib/smplos-pkgbuild.sh" || exit 1

installed=$(smplos_pkg_version eww-smplos)
[[ -n "$installed" ]] || {
    echo "  ERROR: eww-smplos is missing; retry after its install migration."
    exit 1
}
commit=$(sed -n 's/^_commit=\([0-9a-f]\{7\}\).*/\1/p' "$DIR/PKGBUILD")
revision=$(sed -n 's/^pkgrel=\([0-9]\+\)$/\1/p' "$DIR/PKGBUILD")
[[ -n "$commit" && -n "$revision" ]] || {
    echo "  ERROR: cannot determine required EWW package revision."
    exit 1
}
if [[ "$installed" == *".g$commit-"* ]] &&
    [[ "${installed##*-}" =~ ^[0-9]+$ ]] &&
    (( 10#${installed##*-} >= 10#$revision )); then
    echo "  eww-smplos already includes the IPC response repair."
    exit 0
fi

echo "  Building EWW with bounded, verified IPC replies (about 5-10 minutes)..."
smplos_pkgbuild_install "$DIR" --ask 4 || {
    echo "  ERROR: EWW repair was not installed; migration remains pending."
    exit 1
}
installed=$(smplos_pkg_version eww-smplos)
[[ "$installed" == *".g$commit-$revision" ]] || {
    echo "  ERROR: installed EWW revision does not match the repaired package."
    exit 1
}
echo "  EWW repair installed; post-deploy activates it without restarting the desktop."
