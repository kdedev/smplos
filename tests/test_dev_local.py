"""Exercise local-deploy fragments in temporary homes, never as root."""

import os
from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "release/dev-local.sh").read_text()


class LocalDeployTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.home = Path(temporary.name)
        (self.home / ".config/smplos/current").mkdir(parents=True)
        (self.home / ".config/smplos/current/theme.name").write_text("grafium\n")
        theme = self.home / ".config/smplos/current/theme"
        theme.mkdir()
        (theme / "colors.toml").write_text('background = "#101010"\n')
        self.env = {**os.environ, "USER_HOME": str(self.home)}

    def run_fragment(self, fragment, **env):
        prelude = """
set -euo pipefail
log() { echo "$*"; }
warn() { echo "$*"; }
err() { echo "$*" >&2; }
BOLD= NC=
restart_eww=true
restart_hypr=false
do_component() { [[ "$1" == "${COMPONENT:-eww}" ]]; }
own() { :; }
command() {
    [[ "$*" == "-v chromium" || "$*" == "-v brave" ]] && return 1
    builtin command "$@"
}
run_as_user() {
    echo "as-user $*"
    case "$*" in
        "theme-set "*)
            if [[ "${FAIL_THEME:-0}" != 0 ]]; then
                echo "precise theme failure" >&2
                return "$FAIL_THEME"
            fi ;;
        "bar-ctl reload") return "${FAIL_BAR:-0}" ;;
        "hyprctl reload") return "${FAIL_HYPR:-0}" ;;
    esac
}
"""
        return subprocess.run(["bash", "-c", prelude + fragment],
                              env={**self.env, **env}, capture_output=True, text=True, timeout=5)

    def restart_fragment(self):
        return SOURCE.split("# ── Restart services", 1)[1].split("\n", 1)[1]

    def test_theme_failure_is_visible_and_never_starts_or_reports_success(self):
        result = self.run_fragment(self.restart_fragment(), FAIL_THEME="7")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("precise theme failure", result.stderr)
        self.assertIn("deployment is incomplete", result.stderr)
        self.assertNotIn("All done!", result.stdout)
        self.assertNotIn("bar-ctl start", result.stdout)
        self.assertNotIn("kill", result.stdout)

    def test_success_uses_existing_daemon_without_parallel_start(self):
        result = self.run_fragment(self.restart_fragment())
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("as-user theme-set 'grafium'", result.stdout)
        self.assertIn("All done!", result.stdout)
        self.assertNotIn("bar-ctl start", result.stdout)
        self.assertNotIn("killall", SOURCE)

    def test_missing_current_theme_reload_failure_stops_deployment(self):
        (self.home / ".config/smplos/current/theme.name").unlink()
        result = self.run_fragment(self.restart_fragment(), FAIL_BAR="42")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("EWW reload failed", result.stderr)
        self.assertNotIn("All done!", result.stdout)

    def test_eww_copy_preserves_palette_icons_and_custom_files(self):
        target = self.home / ".config/eww"
        target.mkdir()
        for name in ("theme-colors.scss", "theme-colors.yuck", "custom.yuck"):
            (target / name).write_text("keep " + name)
        (target / "icons").mkdir()
        (target / "icons/user.svg").write_text("keep icon")
        fragment = SOURCE.split("# ── EWW ", 1)[1].split("\n", 1)[1]
        fragment = fragment.split("# ── Icons ", 1)[0]
        result = self.run_fragment(fragment, SRC_DIR=str(ROOT / "src"))
        self.assertEqual(result.returncode, 0, result.stderr)
        for name in ("theme-colors.scss", "theme-colors.yuck", "custom.yuck"):
            self.assertEqual((target / name).read_text(), "keep " + name)
        self.assertEqual((target / "icons/user.svg").read_text(), "keep icon")
        self.assertEqual((target / "eww.scss").read_bytes(),
                         (ROOT / "src/shared/eww/eww.scss").read_bytes())


if __name__ == "__main__":
    unittest.main()
