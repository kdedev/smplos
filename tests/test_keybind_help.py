import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import time
import unittest


ROOT = Path(__file__).resolve().parents[1]
HELP = ROOT / "src/shared/bin/keybind-help"


class KeybindHelpTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.home = Path(self.tmp.name)
        config = self.home / ".config/smplos"
        config.mkdir(parents=True)
        (config / "bindings.conf").write_text(
            "bindd = SUPER SHIFT, S, Save screen, exec, screenshot\n"
            "bindd = SUPER, SPACE, Start menu, exec, launcher\n"
            "bindd = SUPER, W, Close window, killactive,\n"
            "bindd = SUPER ALT, S, Move window to scratchpad, movetoworkspacesilent, special:scratchpad\n"
            "bindd = SUPER, S, Toggle scratchpad, togglespecialworkspace, scratchpad\n"
            "bindd = SUPER, RETURN, Terminal, exec, terminal\n"
        )
        messenger = self.home / ".config/hypr"
        messenger.mkdir()
        (messenger / "messenger-bindings.conf").write_text(
            "bindd = SUPER SHIFT, D, Discord messenger, exec, toggle-messenger discord\n"
        )
        self.cache = self.home / ".cache/smplos/keybinds.cache"
        self.cache.parent.mkdir(parents=True)
        self.script = self.home / "keybind-help"
        shutil.copyfile(HELP, self.script)
        self.bin = self.home / "bin"
        self.bin.mkdir()
        rofi = self.bin / "rofi"
        rofi.write_text('#!/bin/bash\nprintf "%s\\0" "$@" > "$ARGS_FILE"\ncat > "$ROWS_FILE"\n')
        rofi.chmod(0o755)
        self.args_file = self.home / "args"
        self.rows_file = self.home / "rows"
        self.env = {
            **os.environ,
            "HOME": str(self.home),
            "PATH": f"{self.bin}:{os.environ['PATH']}",
            "XDG_CACHE_HOME": str(self.home / ".cache"),
            "ARGS_FILE": str(self.args_file),
            "ROWS_FILE": str(self.rows_file),
        }

    def launch(self):
        subprocess.run(
            ["bash", str(self.script)], env=self.env, capture_output=True,
            text=True, check=True, timeout=5,
        )
        args = self.args_file.read_bytes().decode().rstrip("\0").split("\0")
        return args, self.rows_file.read_text()

    def test_substring_options_and_shortcut_order(self):
        args, rows = self.launch()
        self.assertEqual(args[args.index("-matching") + 1], "normal")
        self.assertIn("-i", args)
        self.assertIn("-tokenize", args)
        self.assertIn("-no-sort", args)
        self.assertNotIn("fuzzy", args)
        self.assertNotIn("fzf", args)
        matches = [row for row in rows.splitlines() if "super+s" in row.lower()]
        self.assertEqual(matches[0].split(maxsplit=1), ["Super+S", "Toggle scratchpad"])
        self.assertEqual(len(matches), 4)
        self.assertIn("Discord messenger", rows)
        self.assertNotIn("Super+Alt+S", "\n".join(matches))

    def test_cache_follows_user_cache_home(self):
        self.env["XDG_CACHE_HOME"] = str(self.home / "custom-cache")
        self.launch()
        self.assertTrue((self.home / "custom-cache/smplos/keybinds.cache").is_file())
        self.assertFalse(self.cache.exists())

    def test_script_update_rebuilds_existing_cache(self):
        self.cache.write_text("stale fuzzy-search listing\n")
        older = time.time() - 10
        for path in (self.script, self.home / ".config/smplos/bindings.conf",
                     self.home / ".config/hypr/messenger-bindings.conf"):
            os.utime(path, (older, older))
        newest = time.time() + 1
        os.utime(self.script, (newest, newest))
        _, rows = self.launch()
        self.assertNotIn("stale fuzzy-search listing", rows)
        self.assertIn("Toggle scratchpad", rows)

    def test_os_update_installs_help_without_overwriting_user_bindings(self):
        repo = self.home / "repo"
        source = repo / "src/shared/bin/keybind-help"
        source.parent.mkdir(parents=True)
        shutil.copyfile(HELP, source)
        installed = self.home / "installed-keybind-help"
        installed.write_text("old launcher\n")
        bindings = self.home / ".config/smplos/bindings.conf"
        original_bindings = bindings.read_bytes()
        updater = (ROOT / "src/shared/bin/smplos-os-update").read_text()
        sync = updater.split("sync_scripts() {", 1)[1].split("\n}\n", 1)[0]
        # Exercise the production sync loop while redirecting its one privileged
        # install to a fixture, never invoking the real updater or sudo.
        script = """
set -euo pipefail
header() { :; }
ok() { :; }
warn() { echo "$*" >&2; exit 1; }
sync_libs() { :; }
cmp() { return 1; }
sudo() {
    [[ "$1" == install && "$2" == -m755 && "$4" == /usr/local/bin/keybind-help ]]
    install "$2" "$3" "$TEST_INSTALLED"
}
sync_scripts() {
""" + sync + "\n}\nsync_scripts\n"
        subprocess.run(
            ["bash", "-c", script],
            env={**self.env, "SMPLOS_REPO": str(repo), "TEST_INSTALLED": str(installed)},
            capture_output=True, text=True, check=True, timeout=5,
        )
        self.assertEqual(installed.read_bytes(), HELP.read_bytes())
        self.assertEqual(installed.stat().st_mode & 0o777, 0o755)
        self.assertEqual(bindings.read_bytes(), original_bindings)

    @unittest.skipUnless(shutil.which("rofi") and os.environ.get("DISPLAY"),
                         "requires Rofi and an X display for noninteractive filtering")
    def test_real_rofi_matches_shortcuts_descriptions_and_multiple_words(self):
        args, rows = self.launch()
        for query, expected in (
            ("sUpEr+S", ["Super+S", "Super+Space", "Super+Shift+D", "Super+Shift+S"]),
            ("terminal", ["Super+Enter"]),
            ("window move", ["Super+Alt+S"]),
            ("Super+S scratch", ["Super+S"]),
            ("spcs", []),
        ):
            with self.subTest(query=query):
                result = subprocess.run(
                    [shutil.which("rofi"), "-no-config", *args, "-filter", query, "-dump"],
                    input=rows, text=True, capture_output=True, check=True, timeout=5,
                )
                self.assertEqual(
                    [row.split()[0] for row in result.stdout.splitlines()], expected,
                )


if __name__ == "__main__":
    unittest.main()
