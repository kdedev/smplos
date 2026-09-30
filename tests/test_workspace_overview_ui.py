"""UI-only checks, including an isolated EWW/Xvfb smoke test when available."""

import json
import math
import os
from pathlib import Path
import re
import secrets
import shutil
import socket
import struct
import subprocess
import time
import unittest


ROOT = Path(__file__).resolve().parents[1]
EWW = ROOT / "src/shared/eww"
OVERVIEW = (EWW / "workspace-overview.yuck").read_text()
MAIN = (EWW / "eww.yuck").read_text()
INITIAL = json.loads(re.search(r":initial '([^']+)'", OVERVIEW)[1])
WATCHERS = ROOT / "src/compositors/hyprland/hypr/popup_watchers.lua"
CLOSE_OVERVIEW = 'eww --config "$HOME/.config/eww" close workspace-overview'
FOCUS_EXPRESSION = re.search(r":focusable \{([^}]+)\}", OVERVIEW)[1]


def definition(source, name):
    start = source.index(f"(defwidget {name} ")
    end = source.find("\n(def", start + 1)
    return source[start:end if end != -1 else len(source)]


def state_for(count):
    """Include negative logical coordinates, vertical offsets and portrait."""
    state = dict(INITIAL, enabled=True, focused_monitor="m1", focused_label="M1")
    monitors = []
    for i in range(count):
        workspaces = [
            dict(id=i * 7 + slot, focused=i == 0 and slot == 1,
                 visible=slot == 1, occupied=slot < 3, windows=int(slot < 3),
                 home=f"m{i + 1}", current_monitor=f"m{i + 1}" if slot == 1 else "",
                 temporary=False, tooltip=f"Workspace {i * 7 + slot}")
            for slot in range(1, 8)
        ]
        monitors.append(dict(
            id=f"m{i + 1}", label=f"M{i + 1}", connector=f"DP-{i + 1}",
            tooltip='Display "identity" ${not_code}', focused=i == 0,
            active_workspace=workspaces[0]["id"], x=(i % 3 - 1) * 1920,
            y=(i // 3 - 1) * 2160, width=1080 if i == 2 else 1920,
            height=1920 if i == 2 else 1080,
            workspaces=workspaces,
        ))
    left = min(m["x"] for m in monitors)
    top = min(m["y"] for m in monitors)
    width = max(m["x"] + m["width"] for m in monitors) - left
    height = max(m["y"] + m["height"] for m in monitors) - top
    scale = min(560 / width, 260 / height)
    for m in monitors:
        m.update(map_x=math.floor((m["x"] - left) * scale),
                 map_y=math.floor((m["y"] - top) * scale),
                 map_width=math.floor(m["width"] * scale),
                 map_height=math.floor(m["height"] * scale))
    state.update(monitors=monitors, current=monitors[0]["workspaces"],
                 map_width=math.ceil(width * scale), map_height=math.ceil(height * scale))
    return state


class WorkspaceOverviewContractTests(unittest.TestCase):
    def test_disabled_state_is_complete(self):
        self.assertEqual(INITIAL, dict(
            enabled=False, error="", assignment="preserve", focused_monitor="", focused_label="",
            current=[], monitors=[], disconnected=[], map_width=560, map_height=260,
        ))

    def test_assignment_mode_is_explained_in_the_overview(self):
        self.assertIn('.assignment // "preserve"', OVERVIEW)
        self.assertIn("Auto: left to right", OVERVIEW)
        self.assertIn("top to bottom", OVERVIEW)
        self.assertIn("Saved homes:", OVERVIEW)

    def test_legacy_style_and_single_bar_are_preserved(self):
        self.assertIn('!workspace-state.enabled && ws-style == "numbers"', MAIN)
        self.assertIn('!workspace-state.enabled && ws-style == "squares"', MAIN)
        self.assertEqual(MAIN.count("(defwindow bar\n"), 1)
        self.assertIn("(deflisten workspaces ", MAIN)
        self.assertIn("(deflisten active-workspace ", MAIN)
        trial = definition(OVERVIEW, "workspace-trial-bar")
        self.assertIn("workspace-state.current", trial)
        self.assertNotIn("ws-style", trial)
        self.assertNotIn("workspace-state.monitors", trial)
        self.assertIn(':text {ws.id}', definition(OVERVIEW, "workspace-chip"))

    def test_bar_chips_select_directly_and_only_monitor_button_opens_overview(self):
        trial = definition(OVERVIEW, "workspace-trial-bar")
        self.assertIn('(box :class "workspace-trial-bar"', trial)
        self.assertIn('(button :class "workspace-overview-toggle"', trial)
        self.assertIn(':onclick "popup-toggle workspace-overview"', trial)
        self.assertNotIn("workspace-ctl enabled", trial)
        self.assertIn("(workspace-bar-choice :ws ws)", trial)
        chip = definition(OVERVIEW, "workspace-bar-choice")
        self.assertIn(':onclick "workspace-ctl select ${ws.id}"', chip)
        self.assertNotIn("popup-toggle", chip)
        self.assertNotIn("eww --config", chip)

    def test_bar_errors_are_visible_and_overview_remains_accessible(self):
        trial = definition(OVERVIEW, "workspace-trial-bar")
        self.assertIn(':class "workspace-bar-error" :text "!"', trial)
        self.assertIn(':visible {workspace-state.error != ""}', trial)
        self.assertIn(':tooltip {workspace-state.error}', trial)
        self.assertIn('workspace-state.focused_label : "WS"', trial)
        self.assertNotIn(":active", trial)

    def test_selection_closes_without_cursor_reentry(self):
        for name, command in [("workspace-choice", "select ${ws.id}"),
                              ("workspace-monitor-row", "focus-monitor ${monitor.id}")]:
            widget = definition(OVERVIEW, name)
            self.assertIn(
                r'eww --config \"$HOME/.config/eww\" close workspace-overview'
                f" && workspace-ctl {command}", widget,
            )
            self.assertNotIn("popup-toggle", widget)
        self.assertIn('popup-toggle workspace-overview', OVERVIEW)
        self.assertIn('popup-toggle --close', OVERVIEW)
        self.assertIn("workspace-overview)", (ROOT / "src/shared/bin/popup-toggle").read_text())

    def test_shapes_theme_and_namespace(self):
        css = (EWW / "eww.scss").read_text()
        self.assertTrue(css.isascii())
        for rule in [".workspace-chip.is-focused", ".workspace-chip.is-visible",
                     ".workspace-occupied.has-windows", ".workspace-chip.is-temporary"]:
            self.assertIn(rule, css)
        self.assertIn(':namespace "eww-workspace-overview"', OVERVIEW)
        for name in ("windows.lua", "windows.conf"):
            self.assertIn("eww-workspace-overview",
                          (ROOT / "src/compositors/hyprland/hypr" / name).read_text())

    def test_map_uses_native_widgets_without_text_injection(self):
        self.assertIn('ltrimstr("M") | tonumber', OVERVIEW)
        self.assertIn(".map_x", OVERVIEW)
        self.assertIn(".map_y", OVERVIEW)
        self.assertIn(":width {floor(w)}", OVERVIEW)
        self.assertIn(":height {floor(h)}", OVERVIEW)
        start = OVERVIEW.index("(literal ")
        literal = OVERVIEW[start:OVERVIEW.index('(label :class "workspace-legend"', start)]
        self.assertNotIn(".connector", literal)
        self.assertNotIn(".tooltip", literal)
        self.assertIn("(for monitor in {workspace-state.monitors}", OVERVIEW)
        self.assertIn(":hscroll true :vscroll false", OVERVIEW)
        self.assertIn(":hscroll false :vscroll true", OVERVIEW)

    def test_escape_uses_supported_focus_and_scoped_lifecycle_hooks(self):
        self.assertIn("jq(overview-state, '.escape_available // false') == true", FOCUS_EXPRESSION)
        for name in ("HYPRLAND_INSTANCE_SIGNATURE", "WAYLAND_DISPLAY", "NIRI_SOCKET"):
            self.assertIn(f'get_env("{name}")', FOCUS_EXPRESSION)
        self.assertNotRegex(OVERVIEW, r":onkey(?:down|up)?\b")
        watcher = WATCHERS.read_text()
        self.assertIn(CLOSE_OVERVIEW, watcher)
        self.assertNotIn("define_submap", watcher)
        self.assertNotIn("hl.timer", watcher)
        self.assertNotIn("layer.interactivity", watcher)
        for event in ("layer.opened", "layer.closed", "config.reloaded"):
            self.assertIn(f'hl.on("{event}"', watcher)


@unittest.skipUnless(shutil.which("lua"), "Lua is required for popup lifecycle validation")
class WorkspaceOverviewEscapeTests(unittest.TestCase):
    def test_escape_bind_tracks_namespace_lifetime_and_reload(self):
        harness = r'''
local layers, binds, events, commands = {}, {}, {}, {}
hl = {
    get_layers = function() return layers end,
    exec_cmd = function(command) commands[#commands + 1] = command end,
    dsp = { exec_cmd = function(command) return command end },
    bind = function(key, callback, options)
        assert(not smplos_workspace_overview_escape_ready, "readiness published before registration")
        local bind = {key=key, callback=callback, options=options, enabled=true}
        function bind:set_enabled(value) self.enabled = value end
        binds[key] = bind
        return bind
    end,
    on = function(name, callback) events[name] = callback; return {} end,
}
local function reload()
    binds, events = {}, {}
    dofile(arg[1])
    assert(smplos_workspace_overview_escape_ready == true)
end
local function overview(address, interactivity)
    return {namespace="eww-workspace-overview", address=address,
            mapped=true, interactivity=interactivity or 1}
end
local function active(value)
    assert(binds.Escape.enabled == value, "incorrect Escape bind lifetime")
end
reload()
active(false)
assert(binds["mouse:272"].options.non_consuming)
assert(not binds.Escape.options.non_consuming, "dismissal must consume Escape")
assert(not binds.Escape.options.release, "consume the press, not just release")
assert(not binds.Escape.options.repeating, "held Escape must not repeat-close")
assert(not binds.Escape.options.locked, "must not intercept lockscreen input")
assert(binds.Escape.options.dont_inhibit)

local other = {namespace="eww-quick-settings", address="other", mapped=true, interactivity=1}
layers = {other}
events["layer.opened"](other)
active(false)
local similar = {namespace="eww-workspace-overview-extra", address="similar", mapped=true}
layers = {other, similar}
events["layer.opened"](similar)
active(false)
local passive = overview("passive", 2)
layers = {other, passive}
events["layer.opened"](passive)
active(true)
events["layer.closed"](passive)
active(false)
local popup = overview("popup")
layers = {other, popup}
events["layer.opened"](popup)
active(true)
binds.Escape.callback()
assert(#commands == 1)
assert(commands[1] == 'eww --config "$HOME/.config/eww" close workspace-overview')

-- Interactivity changes do not emit layer.closed; Escape must still dismiss.
for _, mode in ipairs({2, 0, 1}) do
    popup.interactivity = mode
    local before = #commands
    active(true)
    binds.Escape.callback()
    assert(#commands == before + 1, "mapped overview swallowed Escape after focus-mode change")
end
popup.interactivity = nil
binds.Escape.callback()
assert(#commands == 5, "interactivity is not needed to recognize the mapped overview")
local issued = #commands
events["layer.closed"](other)
active(true)

-- The actual Hyprland closed callback runs while mapped is still true.
events["layer.closed"](popup)
active(false)
layers = {other}
assert(other.mapped, "unrelated popup must be left alone")
binds.Escape.callback()
assert(#commands == issued, "a stale callback must not close an absent overview")

-- External close, EWW reload and EWW crash all unmap the layer.
for _ = 1, 3 do
    layers = {popup}
    events["layer.opened"](popup)
    active(true)
    events["layer.closed"](popup)
    active(false)
    layers = {}
end

-- A compositor config reload must reconstruct scope from live layers.
layers = {popup}
reload()
active(true)
events["config.reloaded"]()
active(true)
layers = {}
events["config.reloaded"]()
active(false)
reload()
active(false)

-- Unmapped/fading layers never capture Escape.
popup.mapped = false
passive.mapped = false
layers = {popup, passive}
events["config.reloaded"]()
active(false)

-- If an old instance closes during replacement, retain the new instance.
popup.mapped = true
local replacement = overview("replacement")
layers = {popup, replacement}
events["layer.opened"](replacement)
events["layer.closed"](popup)
active(true)
layers = {replacement}
events["layer.closed"](replacement)
active(false)
print("popup Escape lifecycle passed")
'''
        result = subprocess.run(["lua", "-", str(WATCHERS)], input=harness,
                                text=True, capture_output=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("popup Escape lifecycle passed", result.stdout)

    def test_unsupported_apis_never_create_a_consuming_escape_bind(self):
        harness = r'''
for _, mode in ipairs({
    "missing-get-layers", "missing-set-enabled", "missing-handle",
    "broken-set-enabled", "broken-get-layers", "invalid-layers",
    "missing-on", "missing-exec", "unsupported-open", "unsupported-close",
}) do
    local binds, events = {}, {}
    smplos_workspace_overview_escape_ready = true
    hl = {
        get_layers = function()
            if mode == "broken-get-layers" then error("unsupported query") end
            if mode == "invalid-layers" then return nil end
            return {}
        end,
        exec_cmd = function() error("unexpected command") end,
        dsp = {exec_cmd = function(command) return command end},
        bind = function(key, callback, options)
            assert(key == "mouse:272", "consuming Escape created on unsupported host: " .. mode)
            assert(smplos_workspace_overview_escape_ready == false)
            local bind = {enabled=true, options=options}
            if mode ~= "missing-set-enabled" then
                function bind:set_enabled(value)
                    if mode == "broken-set-enabled" then error("unsupported setter") end
                    self.enabled = value
                end
            end
            binds[key] = bind
            if mode ~= "missing-handle" then return bind end
        end,
        on = function(name, callback)
            if (mode == "unsupported-open" and name == "layer.opened")
                or (mode == "unsupported-close" and name == "layer.closed") then
                return nil
            end
            events[name] = callback
            return {}
        end,
    }
    if mode == "missing-get-layers" then hl.get_layers = nil end
    if mode == "missing-on" then hl.on = nil end
    if mode == "missing-exec" then hl.exec_cmd = nil end
    dofile(arg[1])
    assert(smplos_workspace_overview_escape_ready == false, mode)
    assert(binds.Escape == nil, mode)
    assert(binds["mouse:272"].enabled and binds["mouse:272"].options.non_consuming, mode)
    for _, callback in pairs(events) do
        callback({namespace="eww-workspace-overview", address="probe"})
    end
end
print("unsupported popup APIs remained safe")
'''
        result = subprocess.run(["lua", "-", str(WATCHERS)], input=harness,
                                text=True, capture_output=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("unsupported popup APIs remained safe", result.stdout)
        self.assertEqual(result.stdout.count("Workspace overview Escape unavailable:"), 10)


@unittest.skipUnless(shutil.which("eww") and shutil.which("Xvfb"),
                     "EWW and Xvfb are required for native UI validation")
class WorkspaceOverviewNativeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.root = ROOT / "tests" / f".w{secrets.token_hex(2)}"
        cls.root.mkdir(mode=0o700)
        cls.addClassCleanup(shutil.rmtree, cls.root)
        cls.config = cls.root / "home/.config/eww"
        cls.config.mkdir(parents=True)
        for directory in ("r", "cache"):
            (cls.root / directory).mkdir(mode=0o700)
        cls.env = dict(os.environ, HOME=str(cls.root / "home"),
                       XDG_RUNTIME_DIR=str(cls.root / "r"),
                       XDG_CACHE_HOME=str(cls.root / "cache"),
                       XDG_CONFIG_HOME=str(cls.root / "home/.config"),
                       GDK_BACKEND="x11", XDG_SESSION_TYPE="x11", NO_AT_BRIDGE="1")
        for key in ("WAYLAND_DISPLAY", "HYPRLAND_INSTANCE_SIGNATURE", "NIRI_SOCKET",
                    "DBUS_SESSION_BUS_ADDRESS"):
            cls.env.pop(key, None)
        # TCP-only Xvfb avoids all shared /tmp sockets and lock files.
        with socket.socket() as sock:
            sock.bind(("127.0.0.1", 0))
            port = sock.getsockname()[1]
        display = port - 6000
        cls.env["DISPLAY"] = f"127.0.0.1:{display}"
        auth = cls.root / "Xauthority"
        fields = (b"", str(display).encode(), b"MIT-MAGIC-COOKIE-1", secrets.token_bytes(16))
        auth.write_bytes(struct.pack(">H", 65535) + b"".join(
            struct.pack(">H", len(field)) + field for field in fields))
        auth.chmod(0o600)
        cls.env["XAUTHORITY"] = str(auth)
        cls.xlog = (cls.root / "xvfb.log").open("w+")
        cls.addClassCleanup(cls.xlog.close)
        cls.xvfb = subprocess.Popen(
            ["Xvfb", f":{display}", "-nolock", "-nolisten", "unix", "-listen", "tcp",
             "-auth", str(auth), "-screen", "0", "1280x800x24"],
            env=cls.env, stdout=cls.xlog, stderr=subprocess.STDOUT,
        )
        cls.addClassCleanup(cls.stop, cls.xvfb)
        for _ in range(100):
            try:
                with socket.create_connection(("127.0.0.1", port), timeout=0.1):
                    break
            except OSError:
                time.sleep(0.05)
        else:
            raise AssertionError("Isolated Xvfb did not become responsive")

        source = OVERVIEW[OVERVIEW.index("(defwidget"):]
        fixtures = '\n'.join(definition(MAIN, name) for name in (
            "workspaces-widget", "workspace-dot", "workspace-grid", "workspace-sq"))
        (cls.config / "eww.yuck").write_text(
            f"(defvar workspace-state '{json.dumps(INITIAL)}')\n"
            '(defvar ws-style "squares")\n(defvar ws-total 7)\n'
            '(defvar ws-spacing 1)\n(defvar workspaces "[1,2]")\n'
            '(defvar active-workspace 1)\n' + fixtures + "\n" + source +
            '\n(defwindow trial-bar :namespace "eww-workspace-trial-test"'
            ' :geometry (geometry :width "560px" :height "32px" :anchor "top left")'
            ' (workspaces-widget))\n')
        for path in EWW.glob("*.scss"):
            shutil.copy2(path, cls.config / path.name)
        cls.log = (cls.root / "eww.log").open("w+")
        cls.addClassCleanup(cls.log.close)
        cls.daemon = subprocess.Popen(
            ["eww", "--config", str(cls.config), "--no-daemonize", "daemon"],
            env=cls.env, stdout=cls.log, stderr=subprocess.STDOUT,
        )
        cls.addClassCleanup(cls.stop, cls.daemon)
        for _ in range(100):
            if cls.daemon.poll() is not None:
                break
            if cls.eww("ping", check=False).returncode == 0:
                return
            time.sleep(0.05)
        cls.log.flush()
        cls.xlog.flush()
        raise AssertionError("EWW did not start:\n" + (cls.root / "eww.log").read_text()
                             + "\nXvfb:\n" + (cls.root / "xvfb.log").read_text())

    @staticmethod
    def stop(process):
        if process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=5)

    @classmethod
    def eww(cls, *args, check=True):
        return subprocess.run(["eww", "--config", str(cls.config), *args],
                              env=cls.env, capture_output=True, text=True,
                              timeout=10, check=check)

    @classmethod
    def open_overview(cls):
        subprocess.run(["bash", str(ROOT / "src/shared/bin/popup-toggle"), "workspace-overview"],
                       env=cls.env, capture_output=True, text=True, timeout=10, check=True)

    def test_native_layouts_and_live_state_changes(self):
        self.eww("open", "trial-bar")
        self.open_overview()
        for count in (1, 3, 4, 6, 9):
            with self.subTest(monitors=count):
                state = state_for(count)
                state["assignment"] = "spatial-round-robin" if count in (3, 6) else "preserve"
                self.eww("update", "workspace-state=" + json.dumps(state))
                time.sleep(0.08)
                self.assertIn("workspace-overview", self.eww("active-windows").stdout)
                actual = json.loads(self.eww("get", "workspace-state").stdout)
                self.assertEqual(len(actual["monitors"]), count)
        state = state_for(3)
        state["disconnected"] = [dict(id="m10", label="M10", workspace_ids=[64, 65])]
        state["current"][2]["temporary"] = True
        state["error"] = 'Monitor disconnected: "DP-4"'
        self.eww("update", "workspace-state=" + json.dumps(state))
        error_state = dict(INITIAL, enabled=True, error='Invalid workspace profile: "homes"')
        self.eww("update", "workspace-state=" + json.dumps(error_state))
        self.eww("close", "workspace-overview")
        self.open_overview()
        self.assertEqual(json.loads(self.eww("get", "workspace-state").stdout), error_state)
        self.eww("update", "workspace-state=" + json.dumps(INITIAL), "ws-style=numbers")
        self.eww("update", "ws-style=squares")
        time.sleep(0.1)
        self.log.flush()
        log = (self.root / "eww.log").read_text()
        log = re.sub(r"\x1b\[[0-9;]*m", "", log)
        self.assertNotRegex(log, r"(?im)(^error:|^warning:|\b(ERROR|WARN)\s|CRITICAL|panic|Unknown attribute)")
        self.eww("close", "workspace-overview", "trial-bar")

    def test_selection_command_runs_after_native_window_closes(self):
        mock_bin = self.root / "bin"
        mock_bin.mkdir()
        mock = mock_bin / "workspace-ctl"
        mock.write_text(
            '#!/bin/sh\nprintf "%s\\n" "$*" > "$WORKSPACE_UI_CALLS"\n'
            'eww --config "$HOME/.config/eww" active-windows >> "$WORKSPACE_UI_CALLS"\n')
        mock.chmod(0o755)
        calls = self.root / "calls"
        env = dict(self.env, PATH=f"{mock_bin}:{os.environ['PATH']}",
                   WORKSPACE_UI_CALLS=str(calls))
        self.eww("update", "workspace-state=" + json.dumps(state_for(3)))
        self.eww("open", "trial-bar")
        self.open_overview()
        command = re.search(r':onclick "((?:\\.|[^"])*)"',
                            definition(OVERVIEW, "workspace-bar-choice"))[1]
        command = command.replace("${ws.id}", "8")
        subprocess.run(["sh", "-c", command], env=env, timeout=10, check=True)
        lines = calls.read_text().splitlines()
        self.assertEqual(lines[0], "select 8")
        self.assertIn("trial-bar: trial-bar", lines)
        self.assertIn("workspace-overview: workspace-overview", lines)
        self.eww("close", "workspace-overview", "trial-bar")
        for widget, field, value, expected in (
            ("workspace-choice", "ws.id", "8", "select 8"),
            ("workspace-monitor-row", "monitor.id", "m2", "focus-monitor m2"),
        ):
            self.eww("update", "workspace-state=" + json.dumps(state_for(3)))
            self.open_overview()
            command = re.search(r':onclick "((?:\\.|[^"])*)"',
                                definition(OVERVIEW, widget))[1]
            command = command.replace(r'\"', '"').replace("${" + field + "}", value)
            subprocess.run(["sh", "-c", command], env=env, timeout=10, check=True)
            self.assertEqual(calls.read_text().strip(), expected)
            self.assertNotIn("workspace-overview", self.eww("active-windows").stdout)

    def test_escape_close_command_only_closes_overview(self):
        self.eww("open", "trial-bar")
        self.open_overview()
        subprocess.run(["sh", "-c", CLOSE_OVERVIEW], env=self.env, timeout=10, check=True)
        active = self.eww("active-windows").stdout
        self.assertIn("trial-bar: trial-bar", active)
        self.assertNotIn("workspace-overview", active)
        self.eww("close", "trial-bar")

    def test_native_focus_expression_preserves_other_compositors(self):
        scenarios = [
            ("hyprland", dict(HYPRLAND_INSTANCE_SIGNATURE="test",
                              WAYLAND_DISPLAY="wayland-test"), {"escape_available": True}, "true"),
            ("hyprland-disabled", dict(HYPRLAND_INSTANCE_SIGNATURE="test",
                                       WAYLAND_DISPLAY="wayland-test"), {"escape_available": False}, "ondemand"),
            ("hyprland-old-state", dict(HYPRLAND_INSTANCE_SIGNATURE="test",
                                        WAYLAND_DISPLAY="wayland-test"), {}, "ondemand"),
            ("hyprland-null", dict(HYPRLAND_INSTANCE_SIGNATURE="test",
                                   WAYLAND_DISPLAY="wayland-test"), {"escape_available": None}, "ondemand"),
            ("niri", dict(NIRI_SOCKET="test", WAYLAND_DISPLAY="wayland-test"),
             {"escape_available": True}, "ondemand"),
            ("niri-inherited-signature", dict(NIRI_SOCKET="test", WAYLAND_DISPLAY="wayland-test",
                                              HYPRLAND_INSTANCE_SIGNATURE="stale"),
             {"escape_available": True}, "ondemand"),
            ("x11", {}, {"escape_available": True}, "ondemand"),
            ("x11-inherited-signature", dict(HYPRLAND_INSTANCE_SIGNATURE="stale"),
             {"escape_available": True}, "ondemand"),
        ]
        definitions = []
        for name, values, state, _ in scenarios:
            expression = FOCUS_EXPRESSION.replace("overview-state", json.dumps(json.dumps(state)))
            for key in ("HYPRLAND_INSTANCE_SIGNATURE", "WAYLAND_DISPLAY", "NIRI_SOCKET"):
                expression = expression.replace(f'get_env("{key}")', json.dumps(values.get(key, "")))
            definitions.append(f"(defvar focus-{name} {{{expression}}})")
        with (self.config / "eww.yuck").open("a") as source:
            source.write("\n" + "\n".join(definitions) + "\n")
        self.eww("reload")
        for name, _, _, expected in scenarios:
            with self.subTest(compositor=name):
                self.assertEqual(self.eww("get", f"focus-{name}").stdout.strip(), expected)


if __name__ == "__main__":
    unittest.main()
