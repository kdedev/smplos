"""Optional Hyprland window effects: corner shape, shadow tint, inner glow, border animation."""

import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import tomllib
import unittest


SRC = Path(__file__).resolve().parents[1] / "src"
THEMES = SRC / "shared/themes"
TEMPLATE = THEMES / "_templates/hyprland.conf.tpl"
HYPR = SRC / "compositors/hyprland/hypr"

# (colors.toml key, template placeholder, hyprland.conf variable)
EFFECTS = (
    ("rounding_power", "rounding_power", "themeRoundingPower"),
    ("shadow_range", "shadow_range", "themeShadowRange"),
    ("shadow_render_power", "shadow_render_power", "themeShadowRenderPower"),
    ("shadow_color", "shadow_color", "themeShadowColor"),
    ("shadow_color_inactive", "shadow_color_inactive", "themeShadowColorInactive"),
    ("glow", "glow_flag", "themeGlow"),
    ("glow_range", "glow_range", "themeGlowRange"),
    ("glow_render_power", "glow_render_power", "themeGlowRenderPower"),
    ("glow_color", "glow_color", "themeGlowColor"),
    ("glow_color_inactive", "glow_color_inactive", "themeGlowColorInactive"),
    ("border_animation", "border_animation_flag", "themeBorderAnimation"),
)
# Read by the smpl-hud Hyprland plugin, never by the Hyprland configs.
HUD = (
    ("hud_frame", "hud_frame_flag", "themeHudFrame"),
    ("hud_style", "hud_style", "themeHudStyle"),
    ("hud_gap", "hud_gap", "themeHudGap"),
    ("hud_chamfer", "hud_chamfer", "themeHudChamfer"),
    ("hud_line_width", "hud_line_width", "themeHudLineWidth"),
    ("hud_float_glow_range", "hud_float_glow_range", "themeHudFloatGlowRange"),
    ("hud_sweep", "hud_sweep_flag", "themeHudSweep"),
    ("hud_sweep_speed", "hud_sweep_speed", "themeHudSweepSpeed"),
    ("hud_sweep_length", "hud_sweep_length", "themeHudSweepLength"),
)
EFFECT_KEYS = tuple(key for key, _, _ in EFFECTS + HUD)
EFFECT_VARS = tuple(var for _, _, var in EFFECTS + HUD)

# What a theme that sets none of the effect keys must render: the look every
# stock theme had before these keys existed.
CLASSIC = {
    "themeRoundingPower": "2.0",
    "themeShadowRange": "2",
    "themeShadowRenderPower": "3",
    "themeShadowColor": "rgba(1a1a1aee)",
    "themeShadowColorInactive": "rgba(1a1a1aee)",
    "themeGlow": "0",
    "themeGlowRange": "10",
    "themeGlowRenderPower": "3",
    "themeBorderAnimation": "0",
    "themeHudFrame": "0",
    "themeHudStyle": "scifi",
    "themeHudGap": "5",
    "themeHudChamfer": "10",
    "themeHudLineWidth": "3",
    "themeHudFloatGlowRange": "0",
    "themeHudSweep": "0",
    "themeHudSweepSpeed": "80",
    "themeHudSweepLength": "180",
}

# Evaluates the real looknfeel.lua (and theme.lua) against a recording hl stub.
HARNESS = r'''
local root, version = arg[1], arg[2]
package.path = root .. "/?.lua;" .. package.path
local function show(value)
    if type(value) == "table" and value.colors then
        return table.concat(value.colors, ",") .. "@" .. tostring(value.angle)
    end
    return tostring(value)
end
local config
hl = setmetatable({
    config = function(options) if options.decoration then config = options end end,
    animation = function(a)
        print(table.concat({"anim", a.leaf, tostring(a.enabled), tostring(a.speed),
                            tostring(a.bezier), tostring(a.style)}, "\t"))
    end,
    version = function()
        if version == "unavailable" then error("hl.version missing") end
        return version
    end,
}, {__index = function() return function() end end})
dofile(root .. "/looknfeel.lua")
print("value\trounding\t" .. show(config.decoration.rounding))
print("value\trounding_power\t" .. show(config.decoration.rounding_power))
for _, section in ipairs({"shadow", "glow"}) do
    for _, key in ipairs({"enabled", "range", "render_power", "color", "color_inactive"}) do
        print(table.concat({"value", section .. "." .. key,
                            show(config.decoration[section][key])}, "\t"))
    end
end
print("value\tborder\t" .. show(config.general.col.active_border))
'''


def hypr_vars(text):
    return dict(re.findall(r"(?m)^\$(\w+)\s*=\s*(.*?)\s*$", text))


def stock_themes():
    return sorted(path.parent for path in THEMES.glob("*/colors.toml"))


def read_palette(path):
    with open(path, "rb") as source:
        return tomllib.load(source)


def app_corners(theme):
    """Corner radii rofi, niri and the Nemo quick preview got from a generated theme."""
    rofi = re.search(r"border-rad:\s*(\d+)px;", (theme / "smplos-launcher.rasi").read_text())
    niri = re.findall(r"geometry-corner-radius (\d+)", (theme / "niri-theme.kdl").read_text())
    nemo = re.search(
        r"\.nemo-quick-preview decoration \{\s*border-radius: (\d+)px;",
        (theme / "nemo.css").read_text(),
    )
    return rofi.group(1), set(niri), nemo.group(1)


def sets_effect_key(palette):
    return any(re.search(rf"(?m)^{key}\s*=", palette) for key in EFFECT_KEYS)


def alpha(color):
    return int(re.search(r"rgba\(\w{6}(\w{2})\)", color).group(1), 16)


def lua_color(value):
    """The table form theme.lua hands to hl.config, rendered as stops@angle."""
    tokens = value.split()
    if len(tokens) == 1:
        return value
    return ",".join(tokens[:-1]) + "@" + tokens[-1].removesuffix("deg")


class EffectWiringTests(unittest.TestCase):
    def test_every_effect_is_templated_defaulted_and_consumed_in_both_configs(self):
        template = TEMPLATE.read_text()
        legacy_defaults = (HYPR / "theme.conf").read_text()
        lua_defaults = (HYPR / "theme.lua").read_text()
        lua_config = (HYPR / "looknfeel.lua").read_text()
        conf_config = (HYPR / "looknfeel.conf").read_text()
        for _, placeholder, var in EFFECTS:
            with self.subTest(var=var):
                self.assertIn(f"${var} = {{{{ {placeholder} }}}}\n", template)
                self.assertRegex(legacy_defaults, rf"(?m)^\${var}\s*=")
                self.assertRegex(lua_defaults, rf"(?m)^M\.{var}\s*=")
                self.assertRegex(lua_config, rf"\btheme\.{var}\b")
                self.assertRegex(conf_config, rf"\${var}\b")

    def test_hud_variables_are_templated_for_the_hud_plugin(self):
        template = TEMPLATE.read_text()
        for _, placeholder, var in HUD:
            with self.subTest(var=var):
                self.assertIn(f"${var} = {{{{ {placeholder} }}}}\n", template)

    def test_hyprlang_twin_never_lists_the_lua_only_glow_angle_leaf(self):
        conf = (HYPR / "looknfeel.conf").read_text()
        self.assertRegex(
            conf,
            r"(?m)^\s*animation = borderangle, \$themeBorderAnimation, \d+, linear, loop$",
        )
        uncommented = re.sub(r"(?m)#.*$", "", conf)
        self.assertNotIn(
            "glowangle", uncommented,
            "glowangle exists on Hyprland 0.56+ only; the hyprlang config must not list it",
        )


class StockThemeEffectTests(unittest.TestCase):
    def test_every_generated_theme_defines_every_effect_variable(self):
        for theme in stock_themes():
            with self.subTest(theme=theme.name):
                variables = hypr_vars((theme / "hyprland.conf").read_text())
                for var in EFFECT_VARS:
                    self.assertIn(var, variables)
                    self.assertNotIn("{{", variables[var])

    def test_themes_without_effect_keys_keep_the_classic_look(self):
        classic = []
        for theme in stock_themes():
            palette = (theme / "colors.toml").read_text()
            if not sets_effect_key(palette):
                classic.append((theme, palette))
        self.assertGreater(len(classic), 1)
        for theme, palette in classic:
            with self.subTest(theme=theme.name):
                variables = hypr_vars((theme / "hyprland.conf").read_text())
                for var, expected in CLASSIC.items():
                    self.assertEqual(variables[var], expected, var)
                accent = re.search(r'(?m)^accent\s*=\s*"#(\w{6})"', palette).group(1)
                self.assertEqual(variables["themeGlowColor"], f"rgba({accent}b0)")
                self.assertEqual(variables["themeGlowColorInactive"], f"rgba({accent}b0)")

    def test_neon_is_an_octagonal_glowing_gradient_theme(self):
        variables = hypr_vars((THEMES / "neon/hyprland.conf").read_text())
        # rounding_power 1 makes "rounding" a straight 45 degree cut of rounding / 2 - 1 px.
        self.assertEqual(variables["themeRoundingPower"], "1.0")
        self.assertEqual(variables["themeRounding"], "42")
        self.assertEqual((int(variables["themeRounding"]) // 2) - 1, 20)
        self.assertEqual(variables["themeHudFrame"], "1")
        self.assertEqual(variables["themeHudStyle"], "scifi")
        self.assertEqual(variables["themeGlow"], "1")
        self.assertEqual(variables["themeBorderAnimation"], "1")
        self.assertEqual(variables["themeGlowRange"], "12")
        self.assertEqual(variables["themeHudFloatGlowRange"], "24")
        self.assertEqual(variables["themeHudLineWidth"], "3")
        # Wide gaps: the neighbours' outlines are far enough apart for their halos to
        # stay separate (the theme guide: at least hud_line_width + 13 px).
        apart = 2 * (int(variables["themeGapsIn"]) + int(variables["themeBorderSize"])) - 2 * int(variables["themeHudGap"])
        self.assertGreaterEqual(apart, int(variables["themeHudLineWidth"]) + 13)
        self.assertEqual(apart, 28)
        # A slow, long drift: tens of seconds per lap, a glow a few windows' corners long.
        self.assertEqual(variables["themeHudSweep"], "1")
        self.assertTrue(40 <= int(variables["themeHudSweepSpeed"]) <= 100)
        self.assertTrue(120 <= int(variables["themeHudSweepLength"]) <= 300)
        for var in ("activeBorderColor", "inactiveBorderColor", "themeGlowColor"):
            self.assertGreaterEqual(variables[var].count("rgba("), 2, var)
            self.assertRegex(variables[var], r"\d+deg$", var)
        for strong, dim in (
            ("themeShadowColor", "themeShadowColorInactive"),
            ("themeGlowColor", "themeGlowColorInactive"),
        ):
            self.assertLess(alpha(variables[dim]), alpha(variables[strong]), dim)

    def test_app_corners_follow_rounding_unless_a_theme_sets_app_rounding(self):
        for theme in stock_themes():
            palette = (theme / "colors.toml").read_text()
            rounding = re.search(r'(?m)^rounding\s*=\s*"(\d+)"', palette)
            override = re.search(r'(?m)^app_rounding\s*=\s*"(\d+)"', palette)
            with self.subTest(theme=theme.name):
                self.assertIsNotNone(rounding)
                expected = (override or rounding).group(1)
                self.assertEqual(app_corners(theme), (expected, {expected}, expected))

    def test_neon_leaves_the_corner_shape_to_hyprland(self):
        # Rofi, niri and Nemo's preview only round circularly; a token 2 px
        # radius keeps them nearly square so Hyprland's 45 degree cuts stay the
        # one corner shape.
        self.assertEqual(app_corners(THEMES / "neon"), ("2", {"2"}, "2"))
        variables = hypr_vars((THEMES / "neon/hyprland.conf").read_text())
        self.assertEqual(variables["themeRounding"], "42")

    def test_neon_frame_and_glow_use_different_colours(self):
        variables = hypr_vars((THEMES / "neon/hyprland.conf").read_text())

        def hues(*names):
            return {rgb for name in names for rgb in re.findall(r"rgba\((\w{6})\w{2}\)", variables[name])}

        frame = hues("activeBorderColor", "inactiveBorderColor")
        glow = hues("themeShadowColor", "themeShadowColorInactive", "themeGlowColor", "themeGlowColorInactive")
        self.assertTrue(frame and glow)
        self.assertEqual(frame & glow, set(), "the frame and the glow must not share a colour")

    def test_amber_hud_is_a_panel_frame_theme_in_the_amber_palette(self):
        variables = hypr_vars((THEMES / "amber-hud/hyprland.conf").read_text())
        palette = read_palette(THEMES / "amber-hud/colors.toml")
        stock_amber = read_palette(THEMES / "amber/colors.toml")
        # The same text, accent and status colours as Amber; only the backgrounds differ.
        for key in ("foreground", "fg_dim", "muted", "accent", "accent_alt", "danger", "success", "warning", "info"):
            self.assertEqual(palette[key], stock_amber[key], key)
        self.assertNotEqual(palette["background"], stock_amber["background"])
        # The notched outline with runner dots, around octagonal windows.
        self.assertEqual(variables["themeHudFrame"], "1")
        self.assertEqual(variables["themeHudStyle"], "panel")
        self.assertEqual(variables["themeRoundingPower"], "1.0")
        self.assertEqual(variables["themeRounding"], "42")
        self.assertEqual(variables["themeGlow"], "1")
        # The notches need the outline at least 13 px off the window.
        self.assertGreaterEqual(int(variables["themeHudGap"]), 13)
        apart = 2 * (int(variables["themeGapsIn"]) + int(variables["themeBorderSize"])) - 2 * int(variables["themeHudGap"])
        self.assertGreaterEqual(apart, int(variables["themeHudLineWidth"]) + 13)
        # The dots and the travelling glow are the animation; a turning border gradient
        # would make the plugin re-bake the panel at every step.
        self.assertEqual(variables["themeBorderAnimation"], "0")
        self.assertEqual(variables["themeHudSweep"], "1")
        self.assertTrue(40 <= int(variables["themeHudSweepSpeed"]) <= 100)
        for var in ("activeBorderColor", "inactiveBorderColor", "themeGlowColor"):
            self.assertGreaterEqual(variables[var].count("rgba("), 2, var)
            self.assertRegex(variables[var], r"\d+deg$", var)
        for strong, dim in (
            ("themeShadowColor", "themeShadowColorInactive"),
            ("themeGlowColor", "themeGlowColorInactive"),
        ):
            self.assertLess(alpha(variables[dim]), alpha(variables[strong]), dim)
        self.assertEqual(app_corners(THEMES / "amber-hud"), ("2", {"2"}, "2"))

    def test_amber_hud_is_translucent_unlike_opaque_amber(self):
        amber, hud = (
            hypr_vars((THEMES / name / "hyprland.conf").read_text()) for name in ("amber", "amber-hud")
        )
        self.assertEqual(amber["themeHudFrame"], "0")
        self.assertLess(float(hud["themeOpacityActive"]), 1.0)
        self.assertEqual(hud["themeOpacityActive"], hud["themePopupOpacity"])

    def test_amber_hud_borrows_amber_editor_presets_and_icons(self):
        palette = read_palette(THEMES / "amber-hud/colors.toml")
        for key, file in (("app_theme_nvim", "neovim.lua"), ("app_theme_vscode", "vscode.json")):
            with self.subTest(key=key):
                self.assertEqual(palette[key], "amber")
                self.assertTrue((THEMES / palette[key] / file).is_file())
        self.assertEqual((THEMES / "amber-hud/icons.theme").read_text(), (THEMES / "amber/icons.theme").read_text())
        self.assertNotIn("app_theme_logseq", palette)


class ThemeDisplayNameTests(unittest.TestCase):
    def run_script(self, name, home):
        env = {**os.environ, "HOME": str(home), "SMPLOS_PATH": str(SRC / "shared")}
        result = subprocess.run(
            ["bash", str(SRC / "shared/bin" / name)], env=env, capture_output=True, text=True, check=True
        )
        return result.stdout.splitlines()

    def test_hud_stays_an_acronym_in_the_theme_list(self):
        with tempfile.TemporaryDirectory() as home:
            names = self.run_script("theme-list", home)
        self.assertIn("Amber HUD", names)
        self.assertIn("Amber", names)
        self.assertIn("Rose Pine", names)
        self.assertNotIn("Amber Hud", names)

    def test_hud_stays_an_acronym_in_the_current_theme_name(self):
        with tempfile.TemporaryDirectory() as home:
            current = Path(home) / ".config/smplos/current"
            current.mkdir(parents=True)
            for stored, shown in (("amber-hud", "Amber HUD"), ("rose-pine", "Rose Pine"), ("amber", "Amber")):
                with self.subTest(stored=stored):
                    (current / "theme.name").write_text(stored + "\n")
                    self.assertEqual(self.run_script("theme-current", home), [shown])


@unittest.skipUnless(shutil.which("lua"), "Lua runtime unavailable")
class RuntimeEffectTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.home = Path(temporary.name)

    def evaluate(self, theme_conf=None, version="0.56.2"):
        config = self.home / ".config/hypr"
        shutil.rmtree(config, ignore_errors=True)
        if theme_conf is not None:
            config.mkdir(parents=True)
            (config / "theme.conf").write_text(theme_conf)
        result = subprocess.run(
            ["lua", "-", str(HYPR), version], input=HARNESS, text=True,
            capture_output=True, timeout=10,
            env={"HOME": str(self.home), "PATH": os.environ.get("PATH", "")},
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        values, animations = {}, {}
        for line in result.stdout.splitlines():
            kind, name, *rest = line.split("\t")
            if kind == "anim":
                animations[name] = dict(zip(("enabled", "speed", "bezier", "style"), rest))
            else:
                values[name] = rest[0]
        return values, animations

    def test_every_stock_theme_reaches_the_lua_config_unchanged(self):
        for theme in stock_themes():
            with self.subTest(theme=theme.name):
                text = (theme / "hyprland.conf").read_text()
                variables = hypr_vars(text)
                values, animations = self.evaluate(text)
                self.assertEqual(float(values["rounding"]), float(variables["themeRounding"]))
                self.assertEqual(float(values["rounding_power"]), float(variables["themeRoundingPower"]))
                self.assertEqual(values["shadow.enabled"], "true")
                self.assertEqual(values["shadow.range"], variables["themeShadowRange"])
                self.assertEqual(values["shadow.render_power"], variables["themeShadowRenderPower"])
                self.assertEqual(values["shadow.color"], lua_color(variables["themeShadowColor"]))
                self.assertEqual(
                    values["shadow.color_inactive"], lua_color(variables["themeShadowColorInactive"]),
                )
                self.assertEqual(
                    values["glow.enabled"], "true" if variables["themeGlow"] == "1" else "false",
                )
                self.assertEqual(values["glow.range"], variables["themeGlowRange"])
                self.assertEqual(values["glow.render_power"], variables["themeGlowRenderPower"])
                self.assertEqual(values["glow.color"], lua_color(variables["themeGlowColor"]))
                self.assertEqual(
                    values["glow.color_inactive"], lua_color(variables["themeGlowColorInactive"]),
                )
                self.assertEqual(values["border"], lua_color(variables["activeBorderColor"]))
                self.assertEqual(
                    animations["borderangle"]["enabled"],
                    "true" if variables["themeBorderAnimation"] == "1" else "false",
                )

    def test_missing_or_pre_effects_theme_conf_renders_the_classic_look(self):
        stripped = "".join(
            line + "\n"
            for line in (THEMES / "catppuccin/hyprland.conf").read_text().splitlines()
            if not re.match(r"\$(themeShadow|themeGlow|themeBorderAnimation|themeRoundingPower)", line)
        )
        for name, theme_conf in (("missing", None), ("pre-effects", stripped)):
            with self.subTest(theme_conf=name):
                values, animations = self.evaluate(theme_conf)
                self.assertEqual(float(values["rounding_power"]), 2.0)  # circular corners
                self.assertEqual(values["shadow.range"], CLASSIC["themeShadowRange"])
                self.assertEqual(values["shadow.render_power"], CLASSIC["themeShadowRenderPower"])
                self.assertEqual(values["shadow.color"], CLASSIC["themeShadowColor"])
                self.assertEqual(values["shadow.color_inactive"], CLASSIC["themeShadowColorInactive"])
                self.assertEqual(values["glow.enabled"], "false")
                self.assertEqual(animations["borderangle"]["enabled"], "false")

    def test_border_animation_follows_the_theme_variable_and_is_always_written(self):
        neon = (THEMES / "neon/hyprland.conf").read_text()
        for flag, enabled in (("0", "false"), ("1", "true")):
            text, count = re.subn(
                r"(?m)^(\$themeBorderAnimation\s*=\s*)\d", rf"\g<1>{flag}", neon,
            )
            self.assertEqual(count, 1)
            with self.subTest(enabled=enabled):
                _, animations = self.evaluate(text)
                for leaf in ("borderangle", "glowangle"):
                    self.assertEqual(
                        animations[leaf],
                        {"enabled": enabled, "speed": "80", "bezier": "linear", "style": "loop"},
                    )

    def test_glow_angle_is_only_configured_where_hyprland_knows_it(self):
        # An unknown animation leaf is a config error that pcall cannot hide.
        for version, known in (
            ("0.54.3", False), ("0.55.9", False), ("0.56.0", True), ("0.56.2", True),
            ("0.57.0", True), ("1.0.0", True), ("unknown", False), ("unavailable", False),
        ):
            with self.subTest(version=version):
                _, animations = self.evaluate(version=version)
                self.assertIn("borderangle", animations)
                self.assertEqual("glowangle" in animations, known)


class EffectGenerationTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.source = Path(temporary.name)
        for name in (
            "regen-all-themes.sh", "generate-theme-configs.sh", "regen-nemo-css.py",
            "theme_opacity.py", "check-theme-generation.py",
        ):
            shutil.copy2(SRC / name, self.source / name)
        themes = self.source / "shared/themes"
        shutil.copytree(THEMES / "_templates", themes / "_templates")
        self.theme = themes / "fixture"
        self.theme.mkdir()
        self.palette = self.theme / "colors.toml"
        shutil.copy2(THEMES / "catppuccin/colors.toml", self.palette)
        self.original = self.palette.read_text()

    def generate(self, extra=""):
        self.palette.write_text(self.original + "\n" + extra)
        return subprocess.run(
            ["bash", str(self.source / "regen-all-themes.sh")],
            text=True, capture_output=True, timeout=30,
        )

    def variables(self):
        return hypr_vars((self.theme / "hyprland.conf").read_text())

    def test_omitted_keys_default_to_the_classic_look_with_an_accent_glow(self):
        result = self.generate()
        self.assertEqual(result.returncode, 0, result.stderr)
        variables = self.variables()
        for var, expected in CLASSIC.items():
            self.assertEqual(variables[var], expected, var)
        self.assertEqual(variables["themeGlowColor"], "rgba(89b4fab0)")
        self.assertEqual(variables["themeGlowColorInactive"], "rgba(89b4fab0)")

    def test_overrides_flow_through_and_inactive_colors_follow_the_active_ones(self):
        gradient = "rgba(00f0ffb0) rgba(2f7bff80) 45deg"
        result = self.generate(
            'glow = "true"\nborder_animation = "true"\nshadow_range = "30"\n'
            f'shadow_color = "rgba(00f0ff66)"\nglow_range = "14"\nglow_color = "{gradient}"\n'
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        variables = self.variables()
        self.assertEqual(variables["themeGlow"], "1")
        self.assertEqual(variables["themeBorderAnimation"], "1")
        self.assertEqual(variables["themeShadowRange"], "30")
        self.assertEqual(variables["themeShadowRenderPower"], "3")
        self.assertEqual(variables["themeShadowColor"], "rgba(00f0ff66)")
        self.assertEqual(variables["themeShadowColorInactive"], "rgba(00f0ff66)")
        self.assertEqual(variables["themeGlowRange"], "14")
        self.assertEqual(variables["themeGlowColor"], gradient)
        self.assertEqual(variables["themeGlowColorInactive"], gradient)

    def test_hud_keys_default_off_and_flow_through(self):
        result = self.generate()
        self.assertEqual(result.returncode, 0, result.stderr)
        variables = self.variables()
        self.assertEqual(
            [variables[var] for _, _, var in HUD],
            ["0", "scifi", "5", "10", "3", "0", "0", "80", "180"],
        )
        result = self.generate(
            'hud_frame = "true"\nhud_style = "minimal"\nhud_gap = "6"\n'
            'hud_chamfer = "0"\nhud_line_width = "2.5"\nhud_float_glow_range = "14"\n'
            'hud_sweep = "true"\nhud_sweep_speed = "120"\nhud_sweep_length = "240"\n'
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        variables = self.variables()
        self.assertEqual(
            [variables[var] for _, _, var in HUD],
            ["1", "minimal", "6", "0", "2.5", "14", "1", "120", "240"],
        )

    def test_hud_sweep_numbers_accept_integers_inside_the_plugins_range(self):
        for key, var, values in (
            ("hud_sweep_speed", "themeHudSweepSpeed", ("10", "80", "400", "010", "0099")),
            ("hud_sweep_length", "themeHudSweepLength", ("40", "180", "600", "0040")),
        ):
            for value in values:
                with self.subTest(key=key, value=value):
                    result = self.generate(f'{key} = "{value}"\n')
                    self.assertEqual(result.returncode, 0, result.stderr)
                    # Normalised, so a leading zero can never reach the plugin as octal.
                    self.assertEqual(self.variables()[var], str(int(value)))

    def test_hud_sweep_numbers_reject_everything_else(self):
        bad = ("9", "401", "601", "39", "0", "-5", "2.5", "fast", "", "99999", "0x20", "1e2")
        for key in ("hud_sweep_speed", "hud_sweep_length"):
            low, high = (10, 400) if key == "hud_sweep_speed" else (40, 600)
            for value in bad:
                if value.isdigit() and low <= int(value) <= high:
                    continue
                with self.subTest(key=key, value=value):
                    result = self.generate(f'{key} = "{value}"\n')
                    self.assertNotEqual(result.returncode, 0)
                    self.assertIn(
                        f"fixture: {key} must be an integer from {low} to {high}", result.stderr,
                    )
                    self.assertFalse((self.theme / "hyprland.conf").exists())

    def test_hud_style_accepts_style_names(self):
        for value in ("scifi", "minimal", "my-style_2", "a"):
            with self.subTest(value=value):
                result = self.generate(f'hud_style = "{value}"\n')
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(self.variables()["themeHudStyle"], value)

    def test_hud_style_rejects_everything_else(self):
        for value in ("SciFi", "sci fi", "a/b", "../x", "style!", ""):
            with self.subTest(value=value):
                result = self.generate(f'hud_style = "{value}"\n')
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("fixture: hud_style must be a style name", result.stderr)
                self.assertFalse((self.theme / "hyprland.conf").exists())

    def test_hud_distances_must_be_non_negative_integers(self):
        for key in ("hud_gap", "hud_chamfer", "hud_float_glow_range"):
            for value in ("-1", "2.5", "wide", ""):
                with self.subTest(key=key, value=value):
                    result = self.generate(f'{key} = "{value}"\n')
                    self.assertNotEqual(result.returncode, 0)
                    self.assertIn(
                        f"fixture: {key} must be a non-negative integer", result.stderr,
                    )
                    self.assertFalse((self.theme / "hyprland.conf").exists())

    def test_hud_line_width_accepts_numbers_from_one_to_below_ten(self):
        for value in ("1", "2.5", "3", "9.99"):
            with self.subTest(value=value):
                result = self.generate(f'hud_line_width = "{value}"\n')
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(self.variables()["themeHudLineWidth"], value)

    def test_hud_line_width_rejects_everything_else(self):
        for value in ("0", "0.5", "10", "-1", "3.", "thick", ""):
            with self.subTest(value=value):
                result = self.generate(f'hud_line_width = "{value}"\n')
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("fixture: hud_line_width must be a number from 1 to 9.99", result.stderr)
                self.assertFalse((self.theme / "hyprland.conf").exists())

    def test_rounding_power_accepts_numbers_from_one_to_ten(self):
        for value in ("1", "1.0", "1.5", "2", "9.99", "10", "10.0"):
            with self.subTest(value=value):
                result = self.generate(f'rounding_power = "{value}"\n')
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(self.variables()["themeRoundingPower"], value)

    def test_rounding_power_rejects_everything_else(self):
        for value in ("0", "0.5", "11", "10.5", "-2", "1.", "round", ""):
            with self.subTest(value=value):
                result = self.generate(f'rounding_power = "{value}"\n')
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("fixture: rounding_power must be a number from 1 to 10", result.stderr)
                self.assertFalse((self.theme / "hyprland.conf").exists())

    def test_app_rounding_follows_rounding_unless_the_theme_overrides_it(self):
        result = self.generate()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.variables()["themeRounding"], "8")
        self.assertEqual(app_corners(self.theme), ("8", {"8"}, "8"))
        result = self.generate('app_rounding = "0"\n')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.variables()["themeRounding"], "8")
        self.assertEqual(app_corners(self.theme), ("0", {"0"}, "0"))

    def test_app_rounding_must_be_a_non_negative_integer(self):
        for value in ("-1", "2.5", "square", ""):
            with self.subTest(value=value):
                result = self.generate(f'app_rounding = "{value}"\n')
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("fixture: app_rounding must be a non-negative integer", result.stderr)
                self.assertFalse((self.theme / "hyprland.conf").exists())

    def test_floating_glow_is_independent_of_the_native_glow(self):
        for extra in (
            'glow = "true"\nglow_range = "14"\nhud_float_glow_range = "14"\n',
            'glow = "false"\nglow_range = "7"\nhud_float_glow_range = "14"\n',
            'hud_float_glow_range = "20"\n',
        ):
            with self.subTest(extra=extra):
                result = self.generate(extra)
                self.assertEqual(result.returncode, 0, result.stderr)

    def test_flags_must_be_true_or_false(self):
        for key in ("glow", "border_animation", "hud_frame", "hud_sweep"):
            for value in ("maybe", "yes", "1", "True"):
                with self.subTest(key=key, value=value):
                    result = self.generate(f'{key} = "{value}"\n')
                    self.assertNotEqual(result.returncode, 0)
                    self.assertIn(f"fixture: {key} must be true or false", result.stderr)
                    self.assertFalse((self.theme / "hyprland.conf").exists())


if __name__ == "__main__":
    unittest.main()
