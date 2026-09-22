#!/bin/bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
for tool in pacman pacman-conf vercmp bsdtar repo-add fakeroot; do
    command -v "$tool" >/dev/null || {
        echo "Missing test dependency: $tool (run on an Arch build host)" >&2
        exit 1
    }
done

tmp=$(mktemp -d)
trap 'rm -rf -- "$tmp"' EXIT
export BUILD_VERSION=test
source "$ROOT/src/builder/build.sh"

fail() { echo "FAIL: $*" >&2; exit 1; }
assert_equal() {
    [[ "$1" == "$2" ]] || fail "expected '$2', got '$1'"
}

# Small real packages exercise libalpm metadata parsing, with no installation.
make_package() {
    local directory="$1" name="$2" version="$3" filename="$4"
    mkdir -p "$directory" "$tmp/package"
    printf 'pkgname = %s\npkgver = %s\npkgdesc = fixture\narch = x86_64\nsize = 0\n' \
        "$name" "$version" > "$tmp/package/.PKGINFO"
    bsdtar -caf "$directory/$filename" -C "$tmp/package" .PKGINFO
}

PREBUILT_DIR="$tmp/prebuilt"
OFFLINE_MIRROR_DIR="$tmp/mirror"
mkdir -p "$PREBUILT_DIR" "$OFFLINE_MIRROR_DIR"
AUR_PACKAGES=(nemo-smpl)

make_package "$PREBUILT_DIR" nemo-smpl 1.4.9-1 nemo-smpl-1.4.9-1-x86_64.pkg.tar.xz
make_package "$PREBUILT_DIR" nemo-smpl 1.4.49-1 nemo-smpl-1.4.49-1-x86_64.pkg.tar.zst
make_package "$PREBUILT_DIR" nemo-smpl 1.4.51-1 nemo-smpl-1.4.51-1-x86_64.pkg.tar.zst
make_package "$PREBUILT_DIR" nemo-smpl-debug 99-1 nemo-smpl-debug-99-1-x86_64.pkg.tar.zst
assert_equal "$(smplos_latest_package "$PREBUILT_DIR" nemo-smpl)" \
    "$PREBUILT_DIR/nemo-smpl-1.4.51-1-x86_64.pkg.tar.zst"
assert_equal "$(smplos_latest_package "$PREBUILT_DIR" missing)" ""

# Both package revisions and epochs must use vercmp, not sort -V.
make_package "$tmp/versions" sample 2.0-2 sample-2.0-2-x86_64.pkg.tar.zst
make_package "$tmp/versions" sample 2.0-10 sample-2.0-10-x86_64.pkg.tar.zst
assert_equal "$(smplos_latest_package "$tmp/versions" sample)" \
    "$tmp/versions/sample-2.0-10-x86_64.pkg.tar.zst"
make_package "$tmp/versions" sample 1:1.0-1 sample-1.0-1-x86_64.pkg.tar.xz
assert_equal "$(smplos_latest_package "$tmp/versions" sample)" \
    "$tmp/versions/sample-1.0-1-x86_64.pkg.tar.xz"

mkdir -p "$tmp/broken"
printf 'not a package\n' > "$tmp/broken/nemo-smpl-1.pkg.tar.zst"
if smplos_latest_package "$tmp/broken" nemo-smpl >"$tmp/broken.log" 2>&1; then
    fail "corrupt package accepted"
fi
grep -q 'Cannot read package metadata' "$tmp/broken.log" || fail "missing metadata error"

# Reproduce a same-day cache with an older package and repo database already present.
cp "$PREBUILT_DIR/nemo-smpl-1.4.9-1-x86_64.pkg.tar.xz" "$OFFLINE_MIRROR_DIR/"
cp "$PREBUILT_DIR/nemo-smpl-1.4.49-1-x86_64.pkg.tar.zst" "$OFFLINE_MIRROR_DIR/"
repo-add --quiet "$OFFLINE_MIRROR_DIR/offline.db.tar.gz" \
    "$OFFLINE_MIRROR_DIR/nemo-smpl-1.4.49-1-x86_64.pkg.tar.zst"
mkdir -p "$tmp/db/sync" "$tmp/db/local" "$tmp/cache"
cp "$OFFLINE_MIRROR_DIR/offline.db.tar.gz" "$tmp/db/sync/offline.db"
process_aur_packages > "$tmp/process.log"
[[ -f "$OFFLINE_MIRROR_DIR/nemo-smpl-1.4.51-1-x86_64.pkg.tar.zst" ]] \
    || fail "builder did not inject newest prebuilt"
[[ ! -f "$OFFLINE_MIRROR_DIR/nemo-smpl-debug-99-1-x86_64.pkg.tar.zst" ]] \
    || fail "builder injected debug split"

# The older .xz file is visited after the newer .zst file by the builder's glob.
cp "$tmp/versions/"*.pkg.tar.* "$OFFLINE_MIRROR_DIR/"
(create_repo_database) > "$tmp/repo.log" 2>&1
bsdtar -tf "$OFFLINE_MIRROR_DIR/offline.db.tar.gz" > "$tmp/entries"
grep -qx 'nemo-smpl-1.4.51-1/desc' "$tmp/entries" || fail "repo selected older archive"
grep -qx 'sample-1:1.0-1/desc' "$tmp/entries" || fail "repo ignored package epoch"
if grep -qE 'nemo-smpl-1\.4\.(9|49)-1/' "$tmp/entries"; then
    fail "old version still indexed"
fi
(create_repo_database) >> "$tmp/repo.log" 2>&1
[[ -f "$OFFLINE_MIRROR_DIR/nemo-smpl-1.4.9-1-x86_64.pkg.tar.xz" ]] \
    || fail "older cached archive was removed"
[[ -f "$PREBUILT_DIR/nemo-smpl-1.4.9-1-x86_64.pkg.tar.xz" ]] \
    || fail "older prebuilt archive was removed"

# Exercise the installer's real pacman target resolution in a private database.
# --print is injected unconditionally: this test never installs packages.
printf '[options]\nArchitecture = x86_64\nSigLevel = Never\n[offline]\nServer = file://%s/\n' \
    "$OFFLINE_MIRROR_DIR" > "$tmp/pacman.conf"
pacman-conf() {
    command pacman-conf --config "$tmp/pacman.conf" "$@"
}
sudo() {
    [[ "$1" == pacman ]] || fail "unexpected privileged command: $*"
    printf '%s\n' "$*" >> "$tmp/sudo.calls"
    shift
    fakeroot -- pacman --config "$tmp/pacman.conf" --dbpath "$tmp/db" \
        --cachedir "$tmp/cache" --logfile "$tmp/pacman.log" \
        --print --print-format '%n %v' "$@"
}
printf '# Custom packages\n\n  nemo-smpl  # file manager' > "$tmp/packages.txt"
bsdtar -tf "$tmp/db/sync/offline.db" | grep -qx 'nemo-smpl-1.4.49-1/desc' \
    || fail "test requires a stale installer sync DB"
smplos_install_offline_packages "$tmp/packages.txt" "$OFFLINE_MIRROR_DIR" > "$tmp/install.log"
grep -qx 'nemo-smpl 1.4.51-1' "$tmp/install.log" || fail "installer selected stale sync DB version"

printf '# No custom packages\n' > "$tmp/empty.txt"
assert_equal "$(smplos_install_offline_packages "$tmp/empty.txt")" ""
printf 'missing-package\n' > "$tmp/missing.txt"
if smplos_install_offline_packages "$tmp/missing.txt" "$OFFLINE_MIRROR_DIR" > "$tmp/missing.log" 2>&1; then
    fail "missing repository package was silently skipped"
fi
grep -q 'Failed to install custom packages' "$tmp/missing.log" || fail "missing install error"

calls=$(wc -l < "$tmp/sudo.calls")
printf '[extra]\nServer = https://example.invalid/$repo/os/$arch\n' >> "$tmp/pacman.conf"
if smplos_install_offline_packages "$tmp/packages.txt" "$OFFLINE_MIRROR_DIR" > "$tmp/isolation.log" 2>&1; then
    fail "installer accepted an unrelated online repository"
fi
grep -q 'requires only the offline' "$tmp/isolation.log" || fail "missing repository isolation error"
assert_equal "$(wc -l < "$tmp/sudo.calls")" "$calls"

printf '[options]\nArchitecture = x86_64\n[offline]\nServer = https://example.invalid/\n' > "$tmp/pacman.conf"
if smplos_install_offline_packages "$tmp/packages.txt" "$OFFLINE_MIRROR_DIR" > "$tmp/server.log" 2>&1; then
    fail "installer accepted an online server named offline"
fi
grep -q 'must use the ISO mirror' "$tmp/server.log" || fail "missing mirror isolation error"
assert_equal "$(wc -l < "$tmp/sudo.calls")" "$calls"

mkdir -p "$tmp/no-repo"
printf '[options]\nArchitecture = x86_64\nSigLevel = Never\n[offline]\nServer = file://%s/\n' \
    "$tmp/no-repo" > "$tmp/pacman.conf"
if smplos_install_offline_packages "$tmp/packages.txt" "$tmp/no-repo" > "$tmp/refresh.log" 2>&1; then
    fail "installer ignored failed repository refresh"
fi
grep -q 'Failed to install custom packages' "$tmp/refresh.log" || fail "missing refresh error"

# Guard the installer wiring as well as the helper's behavior.
grep -q 'source "$SMPLOS_PATH/lib/smplos-packages.sh"' "$ROOT/src/shared/installer/install.sh" \
    || fail "installer does not load package helpers"
grep -q 'smplos_install_offline_packages "$aur_list" || exit 1' "$ROOT/src/shared/installer/install.sh" \
    || fail "installer does not propagate package failures"
if grep -q 'find .*pkg\.tar' "$ROOT/src/shared/installer/install.sh"; then
    fail "installer still chooses an unordered package archive"
fi

echo "Package selection regressions passed."
