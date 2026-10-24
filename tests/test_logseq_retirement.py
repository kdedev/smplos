import os
from pathlib import Path
import subprocess
import tempfile
import unittest

from test_app_delivery import function


ROOT = Path(__file__).resolve().parents[1]
MIGRATION = ROOT / "migrations/20261017-180000-replace-logseq-with-grafium.sh"
BUILDER = ROOT / "src/builder/build.sh"


class LogseqRetirementTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.base = Path(self.temp.name)
        self.home = self.base / "home"
        self.bin = self.base / "bin"
        self.bin.mkdir()
        self.home.mkdir()
        self.calls = self.base / "calls"
        self.packages = self.base / "packages"
        self.packages.write_text("grafium-bin\nlogseq-desktop-bin\n")
        self.env = {**os.environ, "HOME": str(self.home),
                    "XDG_STATE_HOME": str(self.home / ".local/state"),
                    "PATH": f"{self.bin}:/usr/bin:/bin", "CALLS": str(self.calls),
                    "PACKAGES": str(self.packages), "FIXTURE": str(self.base)}
        self.mock("sudo", '#!/bin/bash\nexec "$@"\n')
        self.mock("pacman", """#!/bin/bash
if [[ "$1" == -Qq ]]; then
    [[ "${FAIL_QUERY:-0}" == 0 ]] || exit 12
    cat "$PACKAGES"
else
    echo "pacman $*" >> "$CALLS"
    [[ "${FAIL_REMOVE:-0}" == 0 ]] || exit 13
    while IFS= read -r name; do
        [[ " $* " == *" $name "* ]] || echo "$name"
    done < "$PACKAGES" > "$PACKAGES.next"
    mv "$PACKAGES.next" "$PACKAGES"
fi
""")
        self.mock("flatpak", """#!/bin/bash
scope="${2#--}"
if [[ "$1" == list ]]; then
    [[ "${FAIL_FLATPAK_QUERY:-0}" == 0 ]] || exit 14
    [[ ! -f "$FIXTURE/$scope.flatpaks" ]] || cat "$FIXTURE/$scope.flatpaks"
else
    echo "flatpak $*" >> "$CALLS"
    [[ "${FAIL_FLATPAK_REMOVE:-0}" == 0 ]] || exit 15
    : > "$FIXTURE/$scope.flatpaks"
fi
""")
        self.mock("bsdtar", '#!/bin/bash\nprintf "%s\\n" "$REPO_ENTRIES"\n')
        self.mock("repo-remove", '#!/bin/bash\necho "repo-remove $*" >> "$CALLS"\nexit "${FAIL_REPO:-0}"\n')

    def mock(self, name, body):
        path = self.bin / name
        path.write_text(body)
        path.chmod(0o755)

    def run_shell(self, script, **env):
        return subprocess.run(["bash", "-c", script], env={**self.env, **env},
                              text=True, capture_output=True, timeout=10)

    def migrate(self, **env):
        source = (MIGRATION.read_text()
                  .replace("/opt/appimages", str(self.base / "appimages"))
                  .replace("/opt/flatpaks", str(self.base / "flatpaks")))
        return self.run_shell(source, **env)

    def test_build_manifests_exclude_logseq_and_grafium_from_every_edition(self):
        for path in (ROOT / "src").rglob("packages*.txt"):
            selected = [line for line in path.read_text().splitlines()
                        if line.strip() and not line.lstrip().startswith("#")]
            self.assertNotIn("logseq", "\n".join(selected).lower(), str(path))
            self.assertNotIn("grafium", "\n".join(selected).lower(), str(path))
        self.assertFalse((ROOT / "src/shared/pkgbuilds/grafium-bin/PKGBUILD").exists())
        self.assertTrue((ROOT / "src/shared/themes/grafium/colors.toml").is_file())
        self.assertIn("prune_retired_note_packages", function(BUILDER, "create_repo_database"))
        self.assertIn('stage_selected_app_formats "$airootfs"', BUILDER.read_text())

    def test_retirement_preserves_all_notes_and_preferences_and_is_idempotent(self):
        personal = {
            ".logseq/config/config.edn": b'{:personal "never replace"}',
            ".logseq/preferences.json": b'{"externals":["my-graph"]}',
            ".logseq/custom.css": b"personal CSS",
            ".var/app/com.logseq.Logseq/config/data.db": b"irreplaceable Flatpak data",
            "my-graph/logseq/config.edn": b"graph config",
            "my-graph/pages/notes.md": b"irreplaceable notes",
        }
        for relative, data in personal.items():
            path = self.home / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)
        for scope in ("user", "system"):
            (self.base / f"{scope}.flatpaks").write_text("com.logseq.Logseq\n")
        image = self.base / "appimages/Logseq.AppImage"
        image.parent.mkdir()
        image.write_bytes(b"disposable program")
        desktop = self.home / ".local/share/applications/Logseq-appimage.desktop"
        desktop.parent.mkdir(parents=True)
        desktop.write_text(f"[Desktop Entry]\nExec={image} --no-sandbox %U\n")
        original = desktop.read_bytes()
        flatpak_desktop = desktop.parent / "com.logseq.Logseq.desktop"
        flatpak_desktop.write_text("[Desktop Entry]\nExec=/usr/bin/flatpak run com.logseq.Logseq %U\n")
        flatpak_original = flatpak_desktop.read_bytes()
        first_boot = self.base / "flatpaks/install-online.txt"
        first_boot.parent.mkdir()
        first_boot.write_text("# keep header\ncom.logseq.Logseq\norg.keepassxc.KeePassXC\n")
        first_boot.chmod(0o640)
        first = self.migrate()
        self.assertEqual(first.returncode, 0, first.stdout + first.stderr)
        self.assertFalse(image.exists())
        self.assertFalse(desktop.exists())
        self.assertFalse(flatpak_desktop.exists())
        backups = list((self.home / ".local/state/smplos/retired-apps").glob("*/Logseq-appimage.desktop"))
        self.assertEqual([path.read_bytes() for path in backups], [original])
        backups = list((self.home / ".local/state/smplos/retired-apps").glob("*/com.logseq.Logseq.desktop"))
        self.assertEqual([path.read_bytes() for path in backups], [flatpak_original])
        self.assertEqual(first_boot.read_text(), "# keep header\norg.keepassxc.KeePassXC\n")
        self.assertEqual(first_boot.stat().st_mode & 0o777, 0o640)
        calls = self.calls.read_text()
        self.assertIn("pacman -R --noconfirm -- logseq-desktop-bin\n", calls)
        self.assertNotIn("--delete-data", calls)
        self.assertNotIn("--unused", calls)
        self.assertNotIn("-Rns", calls)
        self.assertIn("flatpak uninstall --user", calls)
        self.assertIn("flatpak uninstall --system", calls)
        second = self.migrate()
        self.assertEqual(second.returncode, 0, second.stdout + second.stderr)
        self.assertEqual(self.calls.read_text(), calls)
        self.assertEqual(self.packages.read_text(), "grafium-bin\n")
        for relative, data in personal.items():
            self.assertEqual((self.home / relative).read_bytes(), data, relative)

    def test_inventory_and_removal_failures_are_visible_and_retryable(self):
        for env in ("FAIL_QUERY", "FAIL_REMOVE", "FAIL_FLATPAK_QUERY", "FAIL_FLATPAK_REMOVE"):
            with self.subTest(failure=env):
                self.packages.write_text("grafium-bin\nlogseq-desktop-bin\n")
                (self.base / "user.flatpaks").write_text("com.logseq.Logseq\n")
                result = self.migrate(**{env: "1"})
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("ERROR:", result.stdout + result.stderr)
                self.assertIn("retry", result.stdout + result.stderr)

    def test_all_known_native_variants_and_debug_companions_are_retired_together(self):
        self.packages.write_text("grafium-bin\nlogseq-git\nlogseq-desktop-bin-debug\nother-notes-app\n")
        result = self.migrate()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(self.packages.read_text(), "grafium-bin\nother-notes-app\n")
        self.assertIn("pacman -R --noconfirm -- logseq-git logseq-desktop-bin-debug", self.calls.read_text())

    def test_first_boot_list_publish_failure_preserves_original_and_cleans_staging(self):
        first_boot = self.base / "flatpaks/install-online.txt"
        first_boot.parent.mkdir()
        first_boot.write_text("com.logseq.Logseq\norg.keepassxc.KeePassXC\n")
        self.mock("tee", "#!/bin/bash\ncat >/dev/null\nexit 17\n")
        result = self.migrate()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Cannot write first-boot list", result.stderr)
        self.assertEqual(first_boot.read_text(), "com.logseq.Logseq\norg.keepassxc.KeePassXC\n")
        self.assertEqual(list(first_boot.parent.glob("install-online.txt.*")), [])

    def test_custom_appimage_launcher_is_not_removed(self):
        desktop = self.home / ".local/share/applications/Logseq-appimage.desktop"
        desktop.parent.mkdir(parents=True)
        desktop.write_text("Exec=/home/me/my-custom-notes-app\n")
        result = self.migrate()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(desktop.read_text(), "Exec=/home/me/my-custom-notes-app\n")

    def test_warm_cache_cannot_reintroduce_logseq_appimage_or_flatpak(self):
        cache = self.base / "cache"
        (cache / "appimages").mkdir(parents=True)
        (cache / "appimages/Logseq.AppImage").write_text("retired binary")
        (cache / "appimages/Keep.AppImage").write_text("selected binary")
        (cache / "flatpaks").mkdir()
        (cache / "flatpaks/install-online.txt").write_text("com.logseq.Logseq\n")
        payload = self.base / "payload"
        (payload / "opt/appimages").mkdir(parents=True)
        (payload / "opt/appimages/Logseq.AppImage").write_text("retired payload")
        source = function(BUILDER, "stage_selected_app_formats") + """
log_info() { :; }; log_error() { echo "$*" >&2; }
CACHE_DIR="$FIXTURE/cache"
APPIMAGE_PACKAGES=('Keep|https://example.invalid/keep')
FLATPAK_PACKAGES=(org.keepassxc.KeePassXC)
stage_selected_app_formats "$FIXTURE/payload" || exit $?
APPIMAGE_PACKAGES=(); FLATPAK_PACKAGES=()
stage_selected_app_formats "$FIXTURE/empty"
"""
        result = self.run_shell(source)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual([path.name for path in (payload / "opt/appimages").iterdir()], ["Keep.AppImage"])
        self.assertEqual((payload / "opt/flatpaks/install-online.txt").read_text(),
                         "org.keepassxc.KeePassXC\n")
        self.assertFalse((self.base / "empty/opt/flatpaks/install-online.txt").exists())
        self.assertEqual(list((self.base / "empty/opt/appimages").iterdir()), [])

    def test_cached_retired_packages_and_repo_records_are_pruned_but_other_packages_remain(self):
        mirror = self.base / "mirror"
        mirror.mkdir()
        retired = mirror / "logseq-desktop-bin-0.10.15-1-x86_64.pkg.tar.zst"
        retired.write_text("retired program")
        Path(f"{retired}.sig").write_text("signature")
        grafium = mirror / "grafium-bin-0.0.188-1-x86_64.pkg.tar.zst"
        grafium.write_text("unbundled program")
        grafium_debug = mirror / "grafium-bin-debug-0.0.188-1-x86_64.pkg.tar.xz"
        grafium_debug.write_text("unbundled debug program")
        Path(f"{grafium}.sig").write_text("signature")
        keep = mirror / "satty-bin-0.20.0-1-x86_64.pkg.tar.zst"
        keep.write_text("selected program")
        (mirror / "offline.db.tar.gz").touch()
        source = function(BUILDER, "prune_retired_note_packages") + """
log_info() { :; }
OFFLINE_MIRROR_DIR="$FIXTURE/mirror"
prune_retired_note_packages
"""
        entries = ("logseq-desktop-bin-0.10.15-1/desc\n"
                   "grafium-bin-0.0.188-1/desc\ngrafium-bin-debug-0.0.188-1/desc")
        result = self.run_shell(source, REPO_ENTRIES=entries)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertFalse(retired.exists())
        self.assertFalse(Path(f"{retired}.sig").exists())
        self.assertFalse(grafium.exists())
        self.assertFalse(grafium_debug.exists())
        self.assertFalse(Path(f"{grafium}.sig").exists())
        self.assertEqual(keep.read_text(), "selected program")
        self.assertIn("logseq-desktop-bin", self.calls.read_text())
        self.assertIn("grafium-bin\n", self.calls.read_text())
        self.assertIn("grafium-bin-debug\n", self.calls.read_text())
        result = self.run_shell(source, REPO_ENTRIES="logseq-desktop-bin-0.10.15-1/desc", FAIL_REPO="1")
        self.assertNotEqual(result.returncode, 0)


if __name__ == "__main__":
    unittest.main()
