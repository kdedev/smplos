import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import time
import unittest


ROOT = Path(__file__).resolve().parents[1]
BIN = ROOT / "src/shared/bin"
MIGRATION = ROOT / "migrations/20260929-143800-nemo-current-workspace.sh"
MOCK = r"""#!/usr/bin/env python3
import json, os, re, sys, time
from pathlib import Path

name = Path(sys.argv[0]).name
args = sys.argv[1:]
path = Path(os.environ["TEST_STATE"])
state = json.loads(path.read_text())
with open(os.environ["TEST_CALLS"], "a") as log:
    log.write(json.dumps([name, *args]) + "\n")

def save():
    tmp = path.with_suffix(f".{os.getpid()}.tmp")
    tmp.write_text(json.dumps(state))
    tmp.replace(path)

def number(value):
    return int(str(value), 0)

def selected(value):
    return next(w for w in state["windows"] if number(w["id"]) == number(value))

def focus(value):
    window = selected(value)
    state["focused"] = window["id"]
    state["workspace"] = window["workspace"]
    save()

if name == "mock-nemo":
    if state.get("launch_failure"):
        sys.exit(2)
    time.sleep(state.get("delay", 0))
    state = json.loads(path.read_text())
    if "switch_workspace" in state:
        state["workspace"] = state["switch_workspace"]
    if not state.get("no_window"):
        for index in range(state.get("new_count", 1)):
            state["windows"].append({
                "id": hex(0x99 + index),
                "workspace": state.get("new_workspace", state["workspace"]),
                "rank": 0, "class": "nemo",
            })
    save()
elif name == "hyprctl":
    if state.get("bad_json"):
        print("invalid IPC response")
    elif "monitors" in args:
        print(json.dumps([{"focused": True, "activeWorkspace": {"id": state["workspace"]},
                           "specialWorkspace": {"id": 0}}]))
    elif "clients" in args:
        print(json.dumps([
            {"address": w["id"], "class": w["class"], "initialClass": w["class"],
             "workspace": {"id": w["workspace"]}, "focusHistoryID": w["rank"],
             "mapped": True, "pid": 123}
            for w in state["windows"]
        ]))
    elif args[0] == "dispatch":
        if state.get("dispatch_error"):
            print("error: window unavailable")
            sys.exit()
        command = args[1]
        address = re.search(r"address:(0x[0-9a-f]+)", command)
        if ".window.move(" in command:
            selected(address[1])["workspace"] = int(re.search(r"workspace=(-?\d+)", command)[1])
            save()
        elif address:
            focus(address[1])
        print("ok")
    else:
        sys.exit(2)
elif name == "niri":
    if "workspaces" in args:
        print(json.dumps([{"id": state["workspace"], "idx": 1, "is_focused": True}]))
    elif "windows" in args:
        print(json.dumps([
            {"id": number(w["id"]), "app_id": w["class"], "workspace_id": w["workspace"],
             "is_focused": w["id"] == state.get("focused"),
             "focus_timestamp": {"secs": 100 - w["rank"], "nanos": 0}}
            for w in state["windows"]
        ]))
    elif "focus-window" in args:
        focus(args[-1])
    else:
        sys.exit(2)
elif name == "socat":
    request = json.load(sys.stdin)
    action = request["Action"]["MoveWindowToWorkspace"]
    assert action["focus"] is False
    selected(action["window_id"])["workspace"] = action["reference"]["Id"]
    state["niri_move"] = action
    save()
    print('{"Ok":"Handled"}')
elif name == "wmctrl":
    if args == ["-d"]:
        print(f'{state["workspace"]} * geometry viewport workarea desktop')
    elif args == ["-lx"]:
        for w in state["windows"]:
            print(f'{w["id"]} {w["workspace"]} {w["class"]}.{w["class"]} test-host folder')
    elif args[0] == "-ia":
        focus(args[1])
    elif args[0] == "-ir":
        selected(args[1])["workspace"] = int(args[-1])
        save()
    else:
        sys.exit(2)
elif name == "xprop":
    print("_NET_CLIENT_LIST_STACKING(WINDOW): window id # " + ", ".join(
        w["id"] for w in sorted(state["windows"], key=lambda w: w["rank"], reverse=True)))
elif name not in ("notify-send", "workspace-group"):
    sys.exit(2)
"""


def window(address, workspace, rank=0, cls="nemo"):
    return {"id": address, "workspace": workspace, "rank": rank, "class": cls}


class WorkspaceLaunchTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.home = Path(self.tmp.name)
        mocks = self.home / "bin"
        mocks.mkdir()
        for name in ("hyprctl", "niri", "wmctrl", "xprop", "socat",
                     "mock-nemo", "notify-send", "workspace-group"):
            path = mocks / name
            path.write_text(MOCK)
            path.chmod(0o755)
        self.state_path = self.home / "state.json"
        self.calls_path = self.home / "calls.jsonl"
        self.calls_path.touch()
        self.env = {
            **os.environ, "HOME": str(self.home),
            "XDG_RUNTIME_DIR": str(self.home / "runtime"),
            "XDG_CACHE_HOME": str(self.home / "cache"),
            "PATH": f"{mocks}:{BIN}:{os.environ['PATH']}",
            "HYPRLAND_INSTANCE_SIGNATURE": "test-session", "NIRI_SOCKET": "",
            "WAYLAND_DISPLAY": "test-wayland", "DISPLAY": "",
            "TEST_STATE": str(self.state_path), "TEST_CALLS": str(self.calls_path),
        }
        self.set_state(windows=[])

    def set_state(self, **overrides):
        self.state_path.write_text(json.dumps({"workspace": 1, "windows": [], **overrides}))

    def state(self):
        return json.loads(self.state_path.read_text())

    def calls(self):
        return [json.loads(line) for line in self.calls_path.read_text().splitlines()]

    def launch(self, **env):
        return subprocess.run(
            [str(BIN / "focus-or-launch"), "--current-workspace", "nemo", "mock-nemo"],
            env={**self.env, **env}, capture_output=True, text=True, timeout=20,
        )

    def assert_success(self, result):
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_focuses_most_recent_local_window_not_global_match(self):
        self.set_state(windows=[
            window("0x10", 2, 0), window("0x11", 1, 4), window("0x12", 1, 1),
        ])
        self.assert_success(self.launch())
        self.assertEqual(self.state()["focused"], "0x12")
        self.assertEqual(self.state()["workspace"], 1)
        self.assertFalse(any(c[0] in ("mock-nemo", "workspace-group") for c in self.calls()))

    def test_creates_a_new_window_without_moving_the_other_workspace_window(self):
        old = window("0x10", 2)
        self.set_state(windows=[old])
        self.assert_success(self.launch())
        self.assertEqual(self.state()["windows"][0], old)
        self.assertEqual(self.state()["focused"], "0x99")
        self.assertEqual(self.state()["workspace"], 1)
        self.assert_success(self.launch())
        self.assertEqual(sum(c[0] == "mock-nemo" for c in self.calls()), 1)

    def test_moves_only_new_window_if_nemo_maps_on_another_workspace(self):
        old = window("0x10", 2)
        self.set_state(windows=[old], new_workspace=2)
        self.assert_success(self.launch())
        self.assertEqual(self.state()["windows"][0], old)
        self.assertEqual(self.state()["windows"][1]["workspace"], 1)
        self.assertEqual(self.state()["focused"], "0x99")

    def test_does_not_pull_user_back_after_workspace_changes_during_startup(self):
        self.set_state(switch_workspace=3)
        self.assert_success(self.launch())
        self.assertEqual(self.state()["windows"][0]["workspace"], 1)
        self.assertEqual(self.state()["workspace"], 3)
        self.assertNotIn("focused", self.state())

    def test_repeated_presses_during_startup_create_one_window(self):
        self.set_state(delay=0.6)
        process = subprocess.Popen(
            [str(BIN / "focus-or-launch"), "--current-workspace", "nemo", "mock-nemo"],
            env=self.env, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
        )
        try:
            deadline = time.monotonic() + 5
            while not any(c[0] == "mock-nemo" for c in self.calls()):
                self.assertLess(time.monotonic(), deadline)
                time.sleep(0.02)
            self.assert_success(self.launch())
            _, errors = process.communicate(timeout=10)
            self.assertEqual(process.returncode, 0, errors)
            self.assertEqual(sum(c[0] == "mock-nemo" for c in self.calls()), 1)
        finally:
            if process.poll() is None:
                process.terminate()
            process.communicate()

    def test_reports_launcher_and_compositor_failures(self):
        for state in (
            {"launch_failure": True},
            {"bad_json": True},
            {"dispatch_error": True, "windows": [window("0x10", 1)]},
        ):
            with self.subTest(state=state):
                self.set_state(**state)
                result = self.launch()
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("focus-or-launch:", result.stderr)

    def test_ambiguous_new_windows_are_not_moved(self):
        self.set_state(new_workspace=2, new_count=2)
        result = self.launch()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("rather than guessing", result.stderr)
        self.assertTrue(all(w["workspace"] == 2 for w in self.state()["windows"]))

    def test_niri_uses_mru_window_and_stable_workspace_id_for_moves(self):
        env = {"NIRI_SOCKET": "/mock/niri.sock", "HYPRLAND_INSTANCE_SIGNATURE": ""}
        self.set_state(windows=[window("0x10", 2), window("0x11", 1, 3), window("0x12", 1, 1)])
        self.assert_success(self.launch(**env))
        self.assertEqual(self.state()["focused"], "0x12")
        self.set_state(workspace=42, windows=[window("0x10", 7)], new_workspace=7)
        self.assert_success(self.launch(**env))
        self.assertEqual(self.state()["niri_move"]["reference"], {"Id": 42})
        self.assertEqual(self.state()["windows"][0]["workspace"], 7)
        self.assertEqual(self.state()["windows"][1]["workspace"], 42)

    def test_x11_filters_desktops_and_moves_only_new_window(self):
        env = {"HYPRLAND_INSTANCE_SIGNATURE": "", "WAYLAND_DISPLAY": "", "DISPLAY": ":test"}
        self.set_state(windows=[window("0x10", 2), window("0x11", 1, 3), window("0x12", 1, 1)])
        self.assert_success(self.launch(**env))
        self.assertEqual(self.state()["focused"], "0x12")
        self.set_state(windows=[window("0x10", 2)], new_workspace=2)
        self.assert_success(self.launch(**env))
        self.assertEqual(self.state()["windows"][0]["workspace"], 2)
        self.assertEqual(self.state()["windows"][1]["workspace"], 1)

    def test_unscoped_helper_keeps_global_behavior(self):
        self.set_state(windows=[window("0x10", 2)])
        result = subprocess.run(
            [str(BIN / "focus-or-launch"), "nemo", "mock-nemo"],
            env=self.env, capture_output=True, text=True, timeout=5,
        )
        self.assert_success(result)
        self.assertIn(["workspace-group", "2"], self.calls())
        self.assertFalse(any(c[0] == "mock-nemo" for c in self.calls()))

    def test_migration_is_idempotent_and_preserves_custom_commands(self):
        repo = self.home / "migration-repo"
        migration = repo / "migrations" / MIGRATION.name
        migration.parent.mkdir(parents=True)
        shutil.copyfile(MIGRATION, migration)
        library = repo / "src/shared/lib/smplos-session-env.sh"
        library.parent.mkdir(parents=True)
        library.write_text("smplos_have_hyprland() { return 1; }\n")
        original = (
            "# user customizations\n"
            "bindd = SUPER SHIFT, F, File manager, exec, focus-or-launch nemo nemo\n"
            "bindd = SUPER SHIFT ALT, F, Floating, exec, nemo --class nemo-float\n"
            "bindd = SUPER SHIFT, B, Browser, exec, focus-web-browser\n"
        )
        conf = self.home / ".config/smplos/bindings.conf"
        conf.parent.mkdir(parents=True)
        conf.write_text(original)
        custom = self.home / ".config/hypr/bindings.conf"
        custom.parent.mkdir(parents=True)
        custom.write_text("bindd = SUPER SHIFT, F, Custom, exec, thunar\n")
        niri = self.home / ".config/niri/config.kdl"
        niri.parent.mkdir(parents=True)
        niri.write_text(
            'binds {\n    Mod+Shift+f { spawn "sh" "-c" "focus-or-launch nemo nemo"; }\n}\n'
        )
        for _ in range(2):
            result = subprocess.run(
                ["bash", str(migration)], env=self.env, capture_output=True,
                text=True, timeout=5,
            )
            self.assert_success(result)
        self.assertEqual(conf.read_text(), original.replace(
            "focus-or-launch nemo nemo", "focus-or-launch --current-workspace nemo nemo"))
        backups = list(conf.parent.glob("bindings.conf.pre-workspace-nemo.*"))
        self.assertEqual(len(backups), 1)
        self.assertEqual(backups[0].read_text(), original)
        self.assertEqual(custom.read_text(), "bindd = SUPER SHIFT, F, Custom, exec, thunar\n")
        self.assertIn("focus-or-launch --current-workspace nemo nemo", niri.read_text())

    def test_generated_niri_nemo_binding_matches_shared_source(self):
        line = next(line for line in (ROOT / "src/shared/configs/smplos/bindings.conf")
                    .read_text().splitlines() if line.startswith("bindd = SUPER SHIFT, F,"))
        result = subprocess.run(
            ["bash", str(BIN / "bindings-to-niri.sh"), "-", "-"],
            input=line + "\n", capture_output=True, text=True, check=True, timeout=5,
        )
        generated = next(line for line in result.stdout.splitlines() if "Mod+Shift+f " in line)
        self.assertIn(generated, (ROOT / "src/compositors/niri/binds.kdl").read_text())


if __name__ == "__main__":
    unittest.main()
