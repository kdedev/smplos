import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
BINDINGS = ROOT / "src/shared/configs/smplos/bindings.conf"
LOADER = ROOT / "src/compositors/hyprland/hypr/bindings_loader.lua"
MIGRATION = ROOT / "migrations/20261018-054000-scrolling-shortcut-conflicts.sh"
GENERATOR = ROOT / "src/shared/bin/generate-messenger-bindings"
LEGACY = (
    "bindd = SUPER SHIFT, LEFT, Move column left, layoutmsg, movewindow l\n"
    "bindd = SUPER SHIFT, RIGHT, Move column right, layoutmsg, movewindow r\n"
    "bindd = SUPER SHIFT, LEFT, Swap window left, swapwindow, l\n"
    "bindd = SUPER SHIFT, RIGHT, Swap window right, swapwindow, r\n"
    "bindd = SUPER, R, Expand column to fill, layoutmsg, fit expand\n"
    "bindd = SUPER, R, Resize mode (arrow keys to resize - Esc to exit), submap, resize\n"
    "bindd = SUPER, J, Cycle layout orientation (left/top/right/bottom), layoutmsg, orientationnext\n"
)


def entries(text):
    scope = "reset"
    for line in text.splitlines():
        if line.strip().startswith("submap ="):
            scope = line.split("=", 1)[1].strip()
        match = re.match(r"\s*(bind[a-z]*)\s*=\s*(.*)", line)
        if match:
            fields = tuple(field.strip() for field in match[2].split(","))
            flags = match[1][4:]
            yield (scope, tuple(sorted(fields[0].upper().split())),
                   fields[1].upper(), "r" in flags, "m" in flags), fields


class ScrollingBindingTests(unittest.TestCase):
    def test_signal_and_scratchpad_have_the_requested_stock_shortcuts(self):
        bindings = {chord: fields for chord, fields in entries(BINDINGS.read_text())}
        self.assertEqual(
            bindings[("reset", ("SUPER",), "S", False, False)][2:],
            ("Signal", "exec", 'toggle-messenger "signal" signal-desktop'),
        )
        self.assertEqual(
            bindings[("reset", ("CTRL", "SUPER"), "S", False, False)][2:],
            ("Toggle scratchpad", "togglespecialworkspace", "scratchpad"),
        )

    def test_stock_has_no_duplicate_press_chords_in_any_submap(self):
        seen = {}
        for chord, fields in entries(BINDINGS.read_text()):
            self.assertNotIn(chord, seen, f"{fields} conflicts with {seen.get(chord)}")
            seen[chord] = fields

    def test_stock_scrolling_messages_match_hyprland_056_contract(self):
        # CScrollingAlgorithm::layoutMsg in the pinned Hyprland 0.56.2.
        allowed = {"move", "colresize", "fit", "focus", "promote", "consume",
                   "expel", "consume_or_expel", "swapcol", "center",
                   "inhibit_scroll", "fit_into_view"}
        for _, fields in entries(BINDINGS.read_text()):
            if len(fields) >= 5 and fields[3] == "layoutmsg":
                self.assertIn(fields[4].split()[0], allowed, fields)

    @unittest.skipUnless(shutil.which("lua"), "requires Lua")
    def test_actual_loader_keeps_window_column_and_resize_actions_separate(self):
        selected = [line for line in BINDINGS.read_text().splitlines()
                    if re.match(r"bindd = SUPER(?: CTRL)?(?: SHIFT)?, (LEFT|RIGHT|UP|DOWN|R),", line)]
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "bindings.conf"
            path.write_text("\n".join(selected) + "\n")
            result = subprocess.run(
                ["lua", "-", str(LOADER), str(path)], input=r'''
local count, actions = {}, {}
hl = {
  dsp = {
    focus = function(a) return "focus:" .. a.direction end,
    layout = function(a) return "layout:" .. a end,
    submap = function(a) return "submap:" .. a end,
    exec_cmd = function(a) return "exec:" .. a end,
    window = {swap = function(a) return "swap:" .. a.direction end},
  },
  bind = function(key, action)
    count[key] = (count[key] or 0) + 1
    actions[key] = action
  end,
}
dofile(arg[1]).load(arg[2])
for key, n in pairs(count) do assert(n == 1, "duplicate " .. key) end
for key, direction in pairs({LEFT="left", RIGHT="right", UP="up", DOWN="down"}) do
  assert(actions["SUPER + SHIFT + " .. key] == "swap:" .. direction)
end
assert(actions["SUPER + CTRL + SHIFT + LEFT"] == "layout:swapcol l")
assert(actions["SUPER + CTRL + SHIFT + RIGHT"] == "layout:swapcol r")
assert(actions["SUPER + R"] == "layout:fit expand")
assert(actions["SUPER + CTRL + R"] == "submap:resize")
''', text=True, capture_output=True, timeout=10,
            )
            self.assertEqual(result.returncode, 0, result.stderr)

    @unittest.skipUnless(shutil.which("Hyprland"), "requires native Hyprland config verifier")
    def test_native_hyprland_accepts_all_binding_constructors(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "hyprland.lua"
            path.write_text(f"dofile({str(LOADER)!r}).load({str(BINDINGS)!r})\n")
            result = subprocess.run(
                ["Hyprland", "--verify-config", "--config", str(path)],
                capture_output=True, text=True, timeout=20,
            )
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


class ScrollingMigrationTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.home = Path(self.tmp.name) / "home with spaces"
        self.home.mkdir()
        self.primary = self.home / ".config/smplos/bindings.conf"
        self.fallback = self.home / ".config/hypr/bindings.conf"
        for path in (self.primary, self.fallback):
            path.parent.mkdir(parents=True)
        self.env = {
            **os.environ, "HOME": str(self.home),
            "XDG_CONFIG_HOME": str(self.home / ".config"),
            "SMPLOS_PATH": str(self.home / ".local/share/smplos"),
            "WAYLAND_DISPLAY": "", "NIRI_SOCKET": "",
        }

    def migrate(self, check=True):
        return subprocess.run(
            ["bash", str(MIGRATION)], env=self.env, check=check,
            capture_output=True, text=True, timeout=10,
        )

    def test_repairs_both_paths_with_verified_backups_and_is_idempotent(self):
        for path in (self.primary, self.fallback):
            path.write_text(LEGACY)
            path.chmod(0o640)
        self.migrate()
        for path in (self.primary, self.fallback):
            result = path.read_text()
            self.assertIn("SUPER CTRL SHIFT, LEFT, Move column left, layoutmsg, swapcol l", result)
            self.assertIn("SUPER CTRL SHIFT, RIGHT, Move column right, layoutmsg, swapcol r", result)
            self.assertIn("SUPER CTRL, R, Resize mode", result)
            self.assertIn("SUPER, R, Expand column to fill", result)
            self.assertNotIn("orientationnext", result)
            self.assertNotIn("layoutmsg, movewindow", result)
            chords = [chord for chord, _ in entries(result)]
            self.assertEqual(len(chords), len(set(chords)))
            backups = list(path.parent.glob("bindings.conf.pre-scrolling-shortcuts.*"))
            self.assertEqual(len(backups), 1)
            self.assertEqual(backups[0].read_text(), LEGACY)
            self.assertEqual(path.stat().st_mode & 0o777, 0o640)
        before = self.primary.read_bytes()
        self.migrate()
        self.assertEqual(self.primary.read_bytes(), before)
        self.assertEqual(len(list(self.home.rglob("*.pre-scrolling-shortcuts.*"))), 2)

    def test_fresh_or_already_fixed_config_needs_no_backup(self):
        self.migrate()
        self.primary.write_bytes(BINDINGS.read_bytes())
        self.migrate()
        self.assertEqual(self.primary.read_bytes(), BINDINGS.read_bytes())
        self.assertEqual(list(self.home.rglob("*.pre-scrolling-shortcuts.*")), [])

    def test_custom_bindings_submaps_and_destination_chords_are_preserved(self):
        custom = (
            "# personal settings\n"
            "bindd = SUPER CTRL SHIFT, LEFT, Personal action, exec, personal-command\n"
            "bind = SUPER CTRL, R, exec, personal-resize\n"
            "bindd = SUPER, J, Custom orientation, exec, custom-rotate\n"
            "submap = custom\n" + LEGACY + "submap = reset\n"
        )
        self.primary.write_text(custom + LEGACY)
        result = self.migrate()
        self.assertTrue(self.primary.read_text().startswith(custom))
        self.assertIn("WARNING", result.stdout)
        global_lines = self.primary.read_text()[len(custom):]
        self.assertNotIn("Move column left", global_lines)
        self.assertNotIn("Resize mode", global_lines)
        self.assertIn("Swap window left", global_lines)
        self.assertIn("Expand column to fill", global_lines)

    def test_mod_order_case_crlf_and_repeated_legacy_entries(self):
        content = (LEGACY + LEGACY).replace("SUPER SHIFT, LEFT", "SHIFT SUPER, left")
        self.primary.write_bytes(content.replace("\n", "\r\n").encode())
        self.migrate()
        result = self.primary.read_bytes()
        self.assertIn(b"swapcol l\r\n", result)
        self.assertEqual(result.count(b"Move column left"), 1)
        self.assertEqual(result.count(b"Resize mode"), 1)
        self.assertNotIn(b"orientationnext", result)

    def test_symlink_is_not_replaced_or_followed_for_writes(self):
        target = self.home / "dotfiles.conf"
        target.write_text(LEGACY)
        self.primary.symlink_to(target)
        result = self.migrate(check=False)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("symlink", result.stderr)
        self.assertTrue(self.primary.is_symlink())
        self.assertEqual(target.read_text(), LEGACY)

    def test_signal_and_scratchpad_swap_together_without_occupancy_false_positive(self):
        original = (
            "bindd = SUPER, S, Toggle scratchpad, togglespecialworkspace, scratchpad\n"
            'bindd = SUPER CTRL, S, Signal, exec, toggle-messenger "signal" signal-desktop\n'
        )
        self.primary.write_text(original)
        self.migrate()
        result = self.primary.read_text()
        self.assertIn("SUPER CTRL, S, Toggle scratchpad", result)
        self.assertIn("SUPER, S, Signal", result)
        self.assertEqual(len(list(entries(result))), 2)
        self.assertEqual(next(self.primary.parent.glob("*.pre-scrolling-shortcuts.*")).read_text(), original)

    def test_generated_signal_binding_moves_into_shared_config_without_old_alias(self):
        self.primary.write_text("bindd = SUPER, S, Toggle scratchpad, togglespecialworkspace, scratchpad\n")
        messenger = self.fallback.with_name("messenger-bindings.conf")
        messenger.write_text('bindd = SUPER CTRL, S, Signal, exec, toggle-messenger "signal" signal-desktop\n')
        self.migrate()
        self.assertIn("SUPER, S, Signal", self.primary.read_text())
        self.assertIn("SUPER CTRL, S, Toggle scratchpad", self.primary.read_text())
        self.assertNotIn("bindd", messenger.read_text())
        self.migrate()
        self.assertEqual(self.primary.read_text().count("Signal"), 1)

    def test_new_signal_stays_global_without_changing_an_unterminated_submap(self):
        custom = (
            "submap = custom\n"
            'bindd = SUPER, Z, Signal, exec, toggle-messenger "signal" signal-desktop'
        )
        self.primary.write_text(
            "bindd = SUPER, S, Toggle scratchpad, togglespecialworkspace, scratchpad\n" + custom
        )
        self.migrate()
        result = self.primary.read_text()
        self.assertTrue(result.endswith(custom))
        signal = [(chord, fields) for chord, fields in entries(result)
                  if fields[2] == "Signal" and chord[0] == "reset"]
        self.assertEqual(len(signal), 1)
        self.assertEqual(signal[0][0][2], "S")

    @unittest.skipIf(os.geteuid() == 0, "root bypasses fixture directory permissions")
    def test_backup_failure_keeps_the_original_config(self):
        self.primary.write_text(LEGACY)
        self.primary.parent.chmod(0o500)
        try:
            result = self.migrate(check=False)
        finally:
            self.primary.parent.chmod(0o700)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(self.primary.read_text(), LEGACY)
        self.assertEqual(list(self.primary.parent.glob("*.tmp.*")), [])


class MessengerShortcutTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.home = Path(self.tmp.name)
        self.config = self.home / "custom config"
        self.primary = self.config / "smplos/bindings.conf"
        self.fallback = self.config / "hypr/bindings.conf"
        self.output = self.fallback.with_name("messenger-bindings.conf")
        for path in (self.primary, self.fallback):
            path.parent.mkdir(parents=True, exist_ok=True)
        self.primary.write_bytes(BINDINGS.read_bytes())
        (self.primary.parent / "messengers.conf").write_text(
            "Signal | signal | signal-desktop\nTelegram | org.telegram.desktop | telegram-desktop\n"
        )
        self.bin = self.home / "bin"
        self.bin.mkdir()
        for name in ("signal-desktop", "telegram-desktop"):
            path = self.bin / name
            path.write_text("#!/bin/sh\nexit 99\n")
            path.chmod(0o755)
        self.niri = self.home / "smplos/configs/niri"
        self.env = {
            **os.environ, "HOME": str(self.home), "XDG_CONFIG_HOME": str(self.config),
            "SMPLOS_PATH": str(self.home / "smplos"),
            "PATH": f"{self.bin}:/usr/bin:/bin", "WAYLAND_DISPLAY": "", "NIRI_SOCKET": "",
        }

    def generate(self):
        subprocess.run(["bash", str(GENERATOR)], env=self.env, check=True,
                       capture_output=True, text=True, timeout=10)
        return self.output.read_text(), (self.niri / "messenger-binds.kdl").read_text()

    def test_shared_signal_binding_is_not_duplicated_and_niri_uses_super_s(self):
        hypr, niri = self.generate()
        self.assertNotIn(", Signal, exec,", hypr)
        self.assertIn("Super+S", hypr)
        self.assertIn(", Telegram, exec,", hypr)
        self.assertIn('Mod+S hotkey-overlay-title="Signal"', niri)
        self.assertNotIn('Mod+Shift+S hotkey-overlay-title="Signal"', niri)
        before = hypr, niri
        self.assertEqual(self.generate(), before)

    def test_niri_partial_does_not_duplicate_signal_already_in_base(self):
        self.niri.mkdir(parents=True)
        (self.niri / "binds.kdl").write_text('Mod+s hotkey-overlay-title="Signal" { spawn "signal-desktop"; }\n')
        _, niri = self.generate()
        self.assertNotIn('hotkey-overlay-title="Signal"', niri)
        self.assertIn('hotkey-overlay-title="Telegram"', niri)

    def test_primary_custom_binding_wins_over_fallback_and_survives_generation(self):
        original = 'bindd = SUPER CTRL ALT, Z, Signal, exec, toggle-messenger "signal" signal-desktop --profile "work messages"\n'
        self.primary.write_text(original)
        self.fallback.write_text("bindd = SUPER SHIFT, Z, Something else, exec, something\n")
        hypr, niri = self.generate()
        self.assertEqual(self.primary.read_text(), original)
        self.assertNotIn(", Signal, exec,", hypr)
        self.assertIn("Super+Ctrl+Alt+Z", hypr)
        self.assertIn('Mod+Ctrl+Alt+Z hotkey-overlay-title="Signal"', niri)
        self.assertIn(r'--profile \"work messages\"', niri)

    def test_submap_only_binding_does_not_suppress_the_global_shortcut(self):
        self.primary.write_text(
            "submap = custom\n"
            'bindd = SUPER, S, Signal, exec, toggle-messenger "signal" signal-desktop\n'
            "submap = reset\n"
        )
        hypr, _ = self.generate()
        self.assertIn("SUPER SHIFT, S, Signal, exec,", hypr)

    def test_fallback_still_provides_explicit_shortcuts_without_primary(self):
        self.primary.unlink()
        self.fallback.write_text('bindd = SUPER, S, Signal, exec, toggle-messenger signal signal-desktop\n')
        hypr, niri = self.generate()
        self.assertNotIn(", Signal, exec,", hypr)
        self.assertIn('Mod+S hotkey-overlay-title="Signal"', niri)

    def test_unterminated_binding_and_messenger_lines_are_processed(self):
        self.primary.write_text('bindd = SUPER, S, Signal, exec, toggle-messenger "signal" signal-desktop')
        (self.primary.parent / "messengers.conf").write_text("Signal | signal | signal-desktop")
        hypr, niri = self.generate()
        self.assertNotIn(", Signal, exec,", hypr)
        self.assertIn('Mod+S hotkey-overlay-title="Signal"', niri)


if __name__ == "__main__":
    unittest.main()
