"""Theme-colored frosted keypad and scoped blur lifetime; no live IPC."""

import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import time
import tomllib
import unittest


ROOT = Path(__file__).resolve().parents[1]
THEMES = ROOT / "src/shared/themes"


def prepare_sheet(fixture):
    config = fixture.config
    source = (ROOT / "src/shared/eww/eww.yuck").read_text()
    sheet = source[source.index("(defwidget pad-sheet-key"):
                   source.index("(defwindow pad-cheatsheet-passthrough")]
    sample = {
        "title": "Keypad", "keys": [{"control": "key1", "row": 0, "column": 0,
        "bound": True, "active": True, "icon": "", "label": "Cheatsheet", "state": ""}],
        "knobs": [], "layers": [], "options": {"opacity": 0.35},
    }
    shutil.copy2(ROOT / "src/shared/eww/pad-icons.yuck", config / "pad-icons.yuck")
    with (config / "eww.yuck").open("a") as file:
        file.write('\n(include "pad-icons.yuck")\n(defvar pad_icons_font "yes")\n'
                   '(defvar pad_sheet ' + json.dumps(json.dumps(sample)) + ')\n' + sheet)
    return sample


class KeypadThemeTests(unittest.TestCase):
    def test_default_uses_dialog_palette_and_limits_only_keypad_alpha(self):
        css = (ROOT / "src/shared/eww/eww.scss").read_text()
        sheet = css.split("Macro keypad cheatsheet", 1)[1]
        self.assertEqual(set(re.findall(r"#[0-9a-fA-F]{3,8}", sheet)), {"#ffffff"},
                         "only the original white specular highlight is fixed")
        self.assertNotIn("$pad-glass", css)
        self.assertIn("background-color: rgba($material-bg, min(1, min($popup-opacity, 0.35) * $i / 7))", sheet)
        self.assertIn("@if $theme-glass", sheet)
        self.assertNotRegex(sheet, r"(?m)^\s*(opacity|filter|backdrop-filter):")
        yuck = (ROOT / "src/shared/eww/eww.yuck").read_text()
        self.assertIn('"dialog pad-sheet o${', yuck)
        self.assertIn(':width 104 :height 92', yuck)
        self.assertIn(':width 196', yuck)
        self.assertIn('require("keypad_glass")', (ROOT / "src/compositors/hyprland/hypr/windows.lua").read_text())
        self.assertTrue((ROOT / "src/compositors/hyprland/hypr/keypad_glass.lua").exists())

    @unittest.skipUnless(shutil.which("lua"), "Lua required for scoped blur lifecycle")
    def test_missing_blur_lifecycle_api_reports_the_limitation(self):
        result = subprocess.run(
            ["lua", "-", str(ROOT / "src/compositors/hyprland/hypr/keypad_glass.lua")],
            input='hl = {}; dofile(arg[1])\n',
            capture_output=True, text=True, timeout=5,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("Keypad glass blur unavailable", result.stdout)

    @unittest.skipUnless(shutil.which("lua"), "Lua required for scoped blur lifecycle")
    def test_blur_is_borrowed_only_for_the_mapped_sheet_and_restored_safely(self):
        result = subprocess.run(
            ["lua", "-", str(ROOT / "src/compositors/hyprland/hypr/keypad_glass.lua")],
            input=r'''
local events, layers, calls = {}, {}, 0
local blur = {enabled=false, size=0, passes=0}
hl = {
  get_config=function(path) return blur[path:match("[^.]+$")] end,
  config=function(config)
    calls = calls + 1
    for key, value in pairs(config.decoration.blur) do blur[key] = value end
  end,
  get_layers=function() return layers end,
  on=function(event, handler) events[event] = handler; return {} end,
}
local subscriptions = dofile(arg[1])
assert(subscriptions.opened and subscriptions.closed and subscriptions.reloaded)
assert(calls == 0)
local other = {namespace="eww-bar", mapped=true, address="other"}
layers = {other}
events["layer.opened"](other)
assert(calls == 0)
local sheet = {namespace="eww-pad-cheatsheet", mapped=false, address="sheet"}
layers = {other, sheet}
events["layer.opened"](sheet)
assert(calls == 0)
sheet.mapped = true
events["layer.opened"](sheet)
assert(blur.enabled and blur.size == 8 and blur.passes == 2 and calls == 1)
events["layer.opened"](sheet)
assert(calls == 1)
local replacement = {namespace="eww-pad-cheatsheet", mapped=true, address="replacement"}
layers = {other, sheet, replacement}
events["layer.closed"](sheet)
assert(blur.size == 8 and calls == 1)
sheet.mapped = false
events["layer.closed"](replacement)
assert(not blur.enabled and blur.size == 0 and blur.passes == 0)
replacement.mapped = false
sheet.mapped = true
events["layer.opened"](sheet)
blur.size = 12 -- user changed this property while the overlay was open
events["layer.closed"](sheet)
assert(not blur.enabled and blur.size == 12 and blur.passes == 0)
sheet.mapped = false
blur = {enabled=true, size=18, passes=4}
sheet.mapped = true
events["layer.opened"](sheet)
assert(blur.size == 18 and blur.passes == 4)
blur = {enabled=true, size=0, passes=0} -- theme reload replaced its baseline
events["config.reloaded"]()
assert(blur.size == 8 and blur.passes == 2)
events["layer.closed"](sheet) -- crash/close before mapped is cleared
assert(blur.enabled and blur.size == 0 and blur.passes == 0)
sheet.mapped = false
local last_calls = calls
blur = {enabled=true, size=6, passes=3}
events["config.reloaded"]()
assert(calls == last_calls and blur.size == 6 and blur.passes == 3)
sheet.mapped = true -- open sheet survives Lua/config reload
events = {}
local reloaded = dofile(arg[1])
events["config.reloaded"]()
assert(blur.size == 8 and blur.passes == 3)
events["layer.closed"](sheet)
assert(blur.size == 6 and blur.passes == 3)
print("scoped blur lifetime passed")
''', capture_output=True, text=True, timeout=5,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.strip(), "scoped blur lifetime passed")

    def test_frosted_separates_original_silver_material_from_dark_semantic_palette(self):
        colors = tomllib.loads((THEMES / "frosted/colors.toml").read_text())
        self.assertEqual(colors["background"], "#0b2230")
        self.assertEqual(colors["glass_background"], "#dce6f0")
        self.assertEqual(colors["glass_foreground"], "#f4f8ff")
        self.assertEqual(colors["glass_fg_dim"], "#e0eaf5")
        self.assertEqual(colors["glass_accent"], "#d4e8fa")
        self.assertEqual(colors["glass_outline"], "#091422")
        self.assertEqual((colors["blur_size"], colors["blur_passes"]), ("8", "2"))
        self.assertEqual(colors["popup_opacity"], "0.35")
        self.assertEqual(colors["app_background_opacity"], "0.35")
        self.assertFalse((THEMES / "frosted/light.mode").exists())
        self.assertEqual(colors["app_theme_nvim"], "catppuccin")
        self.assertEqual(colors["app_theme_vscode"], "catppuccin")
        result = subprocess.run(
            ["bash", str(ROOT / "src/shared/bin/theme-list")],
            env={**os.environ, "SMPLOS_PATH": str(THEMES.parent)},
            capture_output=True, text=True, timeout=5,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("Frosted", result.stdout.splitlines())
        for path in THEMES.glob("*/colors.toml"):
            if path.parent.name != "frosted":
                self.assertNotEqual(tomllib.loads(path.read_text()).get("glass"), "true")
                palette = (path.parent / "eww-colors.scss").read_text()
                background = tomllib.loads(path.read_text())["background"]
                self.assertIn(f"$theme-glass-bg: {background};", palette)

    def test_silver_sheen_and_edges_restore_reference_without_global_blur_changes(self):
        css = (ROOT / "src/shared/eww/eww.scss").read_text()
        self.assertIn("$theme-glass-bg: $theme-bg !default;", css)
        self.assertIn("$material-bg: $theme-glass-bg;", css)
        self.assertIn("linear-gradient(to bottom, rgba($material-bg, 0.12), rgba($material-bg, 0.04))", css)
        self.assertIn("inset 0 1px rgba(#ffffff, 0.12)", css)
        self.assertIn("0 1px 1px rgba($theme-glass-outline, 0.95), 0 0 1px rgba($theme-glass-outline, 0.95)", css)
        self.assertIn("border-color: rgba($accent, 0.24)", css)
        self.assertIn("border-color: rgba($fg-dim, 0.28)", css)
        generator = (ROOT / "src/generate-theme-configs.sh").read_text()
        self.assertIn('pairs[glass_background]="${pairs[glass_background]:-${pairs[background]}}"', generator)
        self.assertNotIn("glass", (ROOT / "src/compositors/hyprland/hypr/looknfeel.lua").read_text())

    def test_solid_native_palette_and_glass_ink_remain_legible(self):
        from test_theme_alpha import nemo
        colors = tomllib.loads((THEMES / "frosted/colors.toml").read_text())
        for background in ("background", "bg_light", "bg_lighter", "surface"):
            for foreground in ("foreground", "fg_dim", "fg_alt", "accent", "accent_alt",
                               "muted", "autosuggestion", "danger", "success", "warning", "info"):
                with self.subTest(background=background, foreground=foreground):
                    self.assertGreaterEqual(nemo.contrast_ratio(colors[background], colors[foreground]), 4.5)
        self.assertGreaterEqual(nemo.contrast_ratio(colors["selection_background"],
                                                   colors["selection_foreground"]), 4.5)
        for foreground in ("glass_foreground", "glass_fg_dim", "glass_accent"):
            self.assertGreaterEqual(nemo.contrast_ratio(colors["glass_outline"], colors[foreground]), 4.5)

    def test_frosted_solid_backdrop_ignores_fallback_images_and_stale_link(self):
        with tempfile.TemporaryDirectory() as temporary:
            home = Path(temporary)
            current = home / ".config/smplos/current"
            current.mkdir(parents=True)
            shutil.copytree(THEMES / "frosted", current / "theme")
            (current / "theme.name").write_text("frosted\n")
            (current / "background").symlink_to("theme/backgrounds/old-pale.png")
            pictures = home / "Pictures/Wallpapers"
            pictures.mkdir(parents=True)
            (pictures / "pale.png").write_bytes(b"not an image: fallback must not be selected")
            commands = home / "bin"
            commands.mkdir()
            log = home / "wallpaper-call"
            for name, body in (
                ("pgrep", "exit 1"),
                ("swaybg", 'printf "%s\\n" "$*" > "$WALLPAPER_CALL"'),
                ("setsid", 'exec "$@"'),
            ):
                command = commands / name
                command.write_text("#!/bin/bash\n" + body + "\n")
                command.chmod(0o755)
            result = subprocess.run(
                ["bash", str(ROOT / "src/shared/bin/theme-bg-next")],
                env={**os.environ, "HOME": str(home), "PATH": f"{commands}:/usr/bin:/bin",
                     "WAYLAND_DISPLAY": "fixture", "WALLPAPER_CALL": str(log)},
                capture_output=True, text=True, timeout=5,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            for _ in range(100):
                if log.exists() and log.read_text():
                    break
                time.sleep(0.01)
            self.assertEqual(log.read_text().strip(), "-c #0b2230")

    @unittest.skipUnless(shutil.which("lua"), "Lua required for binding translation")
    def test_super_shift_t_dispatches_the_normal_theme_picker(self):
        binding = [line for line in (ROOT / "src/shared/configs/smplos/bindings.conf").read_text().splitlines()
                   if line.startswith("bindd = SUPER SHIFT, T,")]
        self.assertEqual(binding, ["bindd = SUPER SHIFT, T, Theme picker, exec, theme-picker"])
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "bindings.conf"
            path.write_text("\n".join(binding))
            result = subprocess.run(
                ["lua", "-", str(ROOT / "src/compositors/hyprland/hypr/bindings_loader.lua"), str(path)],
                input='hl = {dsp={exec_cmd=function(command) return command end}, '
                      'bind=function(keys, command) assert(keys=="SUPER + SHIFT + T"); '
                      'assert(command=="theme-picker"); print(command) end}\n'
                      'dofile(arg[1]).load(arg[2])\n',
                capture_output=True, text=True, timeout=5,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(result.stdout.strip(), "theme-picker")


@unittest.skipUnless(shutil.which("eww") and shutil.which("Xvfb"), "EWW/Xvfb required")
class KeypadNativeThemeTests(unittest.TestCase):
    def test_actual_silver_material_and_halo_render_over_dark_bright_vivid_scenes(self):
        from test_theme_alpha import nemo
        from test_workspace_overview_ui import WorkspaceOverviewNativeTests as Fixture
        Fixture.setUpClass()
        try:
            prepare_sheet(Fixture)
            shutil.copy2(THEMES / "frosted/eww-colors.scss", Fixture.config / "theme-colors.scss")
            Fixture.eww("reload")
            Fixture.eww("open", "pad-cheatsheet")
            time.sleep(0.15)
            fixture = Fixture("test_native_layouts_and_live_state_changes")
            if not Fixture.probe_available:
                self.skipTest("GTK probe required for actual glass colors")
            panel = fixture.widgets("pad-sheet")[0]
            self.assertAlmostEqual(panel["background"][3], 0.35, places=5)
            for actual, offset in zip(panel["background"][:3], (1, 3, 5)):
                self.assertAlmostEqual(actual, int("#dce6f0"[offset:offset + 2], 16) / 255, places=5)
            labels = [fixture.widgets(css_class)[0] for css_class in
                      ("pad-sheet-label", "pad-sheet-num", "pad-sheet-title")]
            colors = tomllib.loads((THEMES / "frosted/colors.toml").read_text())
            for label in labels:
                self.assertEqual(label["color"][3], 1)
                foreground = "#" + "".join(f"{round(value * 255):02x}" for value in label["color"][:3])
                self.assertEqual(label["background"][3], 0)
                self.assertGreaterEqual(nemo.contrast_ratio(colors["glass_outline"], foreground), 4.5)
            screenshots = []
            for name, backdrop in (("dark", "#0b2230"), ("bright", "#ffffff"), ("vivid", "#00ff80")):
                image = Fixture.root / f"silver-{name}.png"
                response = Fixture.root / "response"
                response.unlink(missing_ok=True)
                (Fixture.root / "request").write_text(f"snapshot\n{image}\n{backdrop}\npad-sheet")
                for _ in range(100):
                    if response.exists():
                        break
                    time.sleep(0.02)
                self.assertTrue(response.exists(), "GTK material snapshot did not finish")
                self.assertTrue(image.read_bytes().startswith(b"\x89PNG\r\n\x1a\n"))
                screenshots.append(image.read_bytes())
                if directory := os.environ.get("KEYPAD_GLASS_ARTIFACTS"):
                    target = Path(directory)
                    target.mkdir(parents=True, exist_ok=True)
                    shutil.copy2(image, target / image.name)
            self.assertEqual(len(set(screenshots)), 3, "scene colors must still contribute")
            self.assert_reload_log(fixture)
        finally:
            Fixture.doClassCleanups()

    def assert_reload_log(self, fixture):
        fixture.log.flush()
        log = (fixture.root / "eww.log").read_bytes()[fixture.log_start:].decode()
        log = re.sub(r"\x1b\[[0-9;]*m", "", log)
        # EWW 78105f3 drops IPC response receivers after 100ms, even when the
        # reload later applies successfully. Keep this known limitation visible.
        response_timeout = (
            r"(?m)^.* ERROR eww::error_handling_ctx\s*> "
            r"Failed to send success response from application thread\n\n"
            r"Caused by:\n    channel closed\n"
        )
        log, count = re.subn(response_timeout, "", log)
        if count:
            print(f"EWW limitation: {count} successful-apply response(s) exceeded the IPC deadline",
                  file=sys.stderr)
        self.assertNotRegex(log, r"(?im)(^error:|^warning:|\b(ERROR|WARN)\s|CRITICAL|panic|Unknown attribute)")

    def test_real_widgets_follow_every_palette_and_switch_back_from_glass(self):
        from test_workspace_overview_ui import WorkspaceOverviewNativeTests as Fixture
        Fixture.setUpClass()
        try:
            config = Fixture.config
            prepare_sheet(Fixture)
            fixture = Fixture("test_native_layouts_and_live_state_changes")
            palettes = sorted(THEMES.glob("*/eww-colors.scss"))
            palettes += [THEMES / "frosted/eww-colors.scss", THEMES / "grafium/eww-colors.scss"]
            for palette in palettes:
                with self.subTest(theme=palette.parent.name):
                    shutil.copy2(palette, config / "theme-colors.scss")
                    Fixture.eww("reload")
                    Fixture.eww("open", "pad-cheatsheet")
                    time.sleep(0.15)
                    self.assert_reload_log(fixture)
                    colors = tomllib.loads((palette.parent / "colors.toml").read_text())
                    if Fixture.probe_available:
                        ink = colors.get("glass_foreground", colors["foreground"])
                        label = fixture.widgets("pad-sheet-label")[0]
                        for actual, offset in zip(label["color"][:3], (1, 3, 5)):
                            self.assertAlmostEqual(actual, int(ink[offset:offset + 2], 16) / 255, places=5)
                        self.assertEqual(label["color"][3], 1)
                        self.assertEqual(label["background"][3], 0)
                        panel = fixture.widgets("pad-sheet")[0]
                        self.assertAlmostEqual(panel["background"][3], min(float(colors["popup_opacity"]), 0.35), places=5)
                        material = colors.get("glass_background", colors["background"])
                        for actual, offset in zip(panel["background"][:3], (1, 3, 5)):
                            self.assertAlmostEqual(actual, int(material[offset:offset + 2], 16) / 255, places=5)
                        self.assertEqual(panel["width"], 150)
                    Fixture.eww("close", "pad-cheatsheet")
            thin_palette = (THEMES / "frosted/eww-colors.scss").read_text().replace(
                "$theme-popup-opacity: 0.35;", "$theme-popup-opacity: 0.20;")
            self.assertIn("$theme-popup-opacity: 0.20;", thin_palette)
            (config / "theme-colors.scss").write_text(thin_palette)
            Fixture.eww("reload")
            Fixture.eww("open", "pad-cheatsheet")
            time.sleep(0.15)
            if Fixture.probe_available:
                self.assertAlmostEqual(fixture.widgets("pad-sheet")[0]["background"][3], 0.20, places=5)
            self.assert_reload_log(fixture)
            full_palette = (THEMES / "frosted/eww-colors.scss").read_text()
            for palette, ink in (
                (re.sub(r"(?m)^\$theme-glass-bg:.*\n", "", full_palette), "#f4f8ff"),
                (full_palette.replace("$theme-glass: true;", "$theme-glass: false;"), "#eaf6fb"),
            ):
                (config / "theme-colors.scss").write_text(palette)
                Fixture.eww("reload")
                time.sleep(0.15)
                if Fixture.probe_available:
                    panel = fixture.widgets("pad-sheet")[0]
                    label = fixture.widgets("pad-sheet-label")[0]
                    for actual, offset in zip(panel["background"][:3], (1, 3, 5)):
                        self.assertAlmostEqual(actual, int("#0b2230"[offset:offset + 2], 16) / 255, places=5)
                    for actual, offset in zip(label["color"][:3], (1, 3, 5)):
                        self.assertAlmostEqual(actual, int(ink[offset:offset + 2], 16) / 255, places=5)
                self.assert_reload_log(fixture)
        finally:
            Fixture.doClassCleanups()

    def test_picker_theme_set_reloads_the_open_overlay_without_another_bar(self):
        from test_workspace_overview_ui import WorkspaceOverviewNativeTests as Fixture
        Fixture.setUpClass()
        try:
            sample = prepare_sheet(Fixture)
            with (Fixture.config / "eww.yuck").open("a") as file:
                file.write('\n(defvar ws-count 4)\n')
            stock = Fixture.root / "stock/themes"
            for name in ("frosted", "grafium", "matrix"):
                theme = stock / name
                theme.mkdir(parents=True)
                for file in ("colors.toml", "eww-colors.scss", "hyprland.conf"):
                    shutil.copy2(THEMES / name / file, theme / file)
            log = Fixture.root / "theme-calls"
            env = {**Fixture.env, "SMPLOS_PATH": str(stock.parent), "THEME_CALLS": str(log)}
            mocks = {
                "pgrep": '[[ "$*" == *eww ]]',
                "rofi": 'cat >> "$THEME_CALLS"; printf "%s\\n" "$PICK_THEME"',
                "theme-set": f'exec bash "{ROOT}/src/shared/bin/theme-set" "$@"',
                "bar-ctl": f'echo "bar-ctl $*" >> "$THEME_CALLS"; exec bash "{ROOT}/src/shared/bin/bar-ctl" "$@"',
            }
            for command in ("theme-bg-next", "theme-set-st", "fish", "gsettings",
                            "hyprctl", "systemctl", "code", "codium", "cursor", "notify-send"):
                mocks[command] = 'echo "' + command + ' $*" >> "$THEME_CALLS"; exit 0'
            for name, body in mocks.items():
                path = Fixture.mock_bin / name
                path.write_text("#!/bin/bash\n" + body + "\n")
                path.chmod(0o755)
            Fixture.eww("reload")
            Fixture.eww("open", "trial-bar")
            Fixture.eww("open", "pad-cheatsheet")
            time.sleep(0.3)
            fixture = Fixture("test_native_layouts_and_live_state_changes")
            for choice in ("Frosted", "Grafium", "Matrix", "Frosted", "Grafium"):
                with self.subTest(choice=choice):
                    result = subprocess.run(
                        ["bash", str(ROOT / "src/shared/bin/theme-picker")],
                        env={**env, "PICK_THEME": choice}, capture_output=True, text=True, timeout=15,
                    )
                    self.assertEqual(result.returncode, 0, result.stderr)
                    name = choice.lower()
                    current = Fixture.root / "home/.config/smplos/current"
                    self.assertEqual((current / "theme.name").read_text().strip(), name)
                    self.assertEqual((Fixture.config / "theme-colors.scss").read_bytes(),
                                     (THEMES / name / "eww-colors.scss").read_bytes())
                    self.assertEqual((Fixture.root / "home/.config/hypr/theme.conf").read_bytes(),
                                     (THEMES / name / "hyprland.conf").read_bytes())
                    if Fixture.probe_available:
                        colors = tomllib.loads((THEMES / name / "colors.toml").read_text())
                        panel = fixture.widgets("pad-sheet")[0]
                        self.assertAlmostEqual(panel["background"][3], min(float(colors["popup_opacity"]), 0.35), places=5)
                        material = colors.get("glass_background", colors["background"])
                        for actual, offset in zip(panel["background"][:3], (1, 3, 5)):
                            self.assertAlmostEqual(actual, int(material[offset:offset + 2], 16) / 255, places=5)
                        ink = colors.get("glass_foreground", colors["foreground"])
                        label = fixture.widgets("pad-sheet-label")[0]
                        for actual, offset in zip(label["color"][:3], (1, 3, 5)):
                            self.assertAlmostEqual(actual, int(ink[offset:offset + 2], 16) / 255, places=5)
                        bars = fixture.widgets("bar-content")
                        self.assertEqual(len(bars), 1)
                        bar_alpha = float(colors["popup_opacity"]) if colors.get("glass") == "true" else 0.95
                        self.assertAlmostEqual(bars[0]["background"][3], bar_alpha, places=5)
                        for actual, offset in zip(bars[0]["background"][:3], (1, 3, 5)):
                            self.assertAlmostEqual(actual, int(material[offset:offset + 2], 16) / 255, places=5)
                        for saved in (0.05, 0.10, 0.125, 0.35, 0.375, 0.70, 1.0):
                            sample["options"]["opacity"] = saved
                            Fixture.eww("update", "pad_sheet=" + json.dumps(sample))
                            time.sleep(0.06)
                            panel = fixture.widgets("pad-sheet")[0]
                            index = round(saved * 20)
                            expected = min(1, min(float(colors["popup_opacity"]), 0.35) * index / 7)
                            self.assertAlmostEqual(panel["background"][3], expected, places=5)
                        sample["options"]["opacity"] = 0.35
                        Fixture.eww("update", "pad_sheet=" + json.dumps(sample))
                    time.sleep(0.3)
                    active = Fixture.eww("active-windows").stdout
                    self.assertEqual(active.count("trial-bar: trial-bar"), 1)
                    self.assertEqual(active.count("pad-cheatsheet: pad-cheatsheet"), 1)
                    self.assertIsNone(Fixture.daemon.poll(), "theme switches restarted EWW")
                    self.assert_reload_log(fixture)
            calls = log.read_text()
            self.assertEqual(calls.count("bar-ctl reload\n"), 5)
            self.assertNotIn("bar-ctl start", calls)
            self.assertIn("Frosted", calls)
        finally:
            Fixture.doClassCleanups()


if __name__ == "__main__":
    unittest.main()
