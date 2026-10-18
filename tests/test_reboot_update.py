"""Reboot callbacks use a fake systemctl and an isolated notification bus."""

import fcntl
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
LIBRARY = ROOT / "src/shared/lib/smplos-reboot.sh"
NOTIFIER = ROOT / "src/shared/bin/smplos-reboot-notify"


class RebootNotificationTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.home = Path(self.temp.name)
        self.bin = self.home / "bin"
        self.bin.mkdir()
        self.marker = self.home / ".local/state/smplos/reboot-pending"
        self.marker.parent.mkdir(parents=True)
        self.marker.write_text(Path("/proc/sys/kernel/random/boot_id").read_text())
        self.env = {
            **os.environ, "HOME": str(self.home), "PATH": f"{self.bin}:/usr/bin:/bin",
            "CALL_LOG": str(self.home / "notifications"), "ACTIONS": '["default","default"]',
            "REBOOT_LOG": str(self.home / "reboots"),
        }
        client = f"""#!{sys.executable}
import json, os, pathlib, sys
log = pathlib.Path(os.environ["CALL_LOG"])
calls = log.read_text().splitlines() if log.exists() else []
with log.open("a") as stream:
    stream.write(json.dumps(sys.argv[1:]) + "\\n")
actions = json.loads(os.environ["ACTIONS"])
if "-A" in sys.argv or "--wait" in sys.argv:
    if os.environ.get("CLIENT_FAIL") == "1":
        sys.exit(1)
    print(actions[len(calls)] if len(calls) < len(actions) else "cancel")
"""
        for name in ("dunstify", "notify-send"):
            self.command(name, client)
        self.command("pgrep", '#!/bin/bash\nexit "${NO_DUNST:-0}"\n')
        self.command("logger", "#!/bin/bash\nexit 0\n")
        self.command("systemctl", '#!/bin/bash\nprintf "%s\\n" "$*" >> "$REBOOT_LOG"\n'
                     'exit "${REBOOT_FAIL:-0}"\n')

    def command(self, name, source):
        target = self.bin / name
        target.write_text(source)
        target.chmod(0o755)

    def run_notifier(self, **env):
        return subprocess.run(["bash", str(NOTIFIER)], env={**self.env, **env},
                              capture_output=True, text=True, timeout=10)

    def check_marker(self, marker=None, boot=None):
        return subprocess.run(
            ["bash", "-c", 'source "$1"; smplos_reboot_pending "$2" "$3"', "_",
             str(LIBRARY), str(marker or self.marker),
             str(boot or "/proc/sys/kernel/random/boot_id")],
            env=self.env, capture_output=True, text=True, timeout=5,
        )

    def test_marker_distinguishes_this_boot_missing_expired_and_corrupt_state(self):
        self.assertEqual(self.check_marker().returncode, 0)
        self.marker.unlink()
        self.assertEqual(self.check_marker().returncode, 1)
        self.marker.write_text("0" * 32 + "\n")
        self.assertEqual(self.check_marker().returncode, 1)
        self.assertEqual(self.run_notifier().returncode, 0)
        self.assertFalse(self.marker.exists())
        for content in ("", "invalid", "0" * 32 + "\nextra"):
            with self.subTest(content=content):
                self.marker.write_text(content)
                result = self.check_marker()
                self.assertEqual(result.returncode, 2, result.stderr)
                self.assertIn("invalid", result.stderr)
                self.assertEqual(self.marker.read_text(), content)
        self.assertFalse(Path(self.env["REBOOT_LOG"]).exists())

    def test_two_explicit_default_actions_are_required_with_both_clients(self):
        for no_dunst in ("0", "1"):
            with self.subTest(no_dunst=no_dunst):
                Path(self.env["CALL_LOG"]).unlink(missing_ok=True)
                Path(self.env["REBOOT_LOG"]).unlink(missing_ok=True)
                result = self.run_notifier(NO_DUNST=no_dunst)
                self.assertEqual(result.returncode, 0, result.stderr)
                calls = [json.loads(line) for line in Path(self.env["CALL_LOG"]).read_text().splitlines()]
                self.assertEqual(len(calls), 2)
                self.assertIn("smplOS: Reboot required", calls[0])
                self.assertIn("Restart smplOS now?", calls[1])
                self.assertTrue(any("default" in arg for arg in calls[0]))
                self.assertIn("Click this confirmation", calls[1][-1])
                self.assertEqual(Path(self.env["REBOOT_LOG"]).read_text(), "reboot\n")
                self.assertTrue(self.marker.exists())
                lock = self.home / ".local/state/smplos/update/update.lock"
                self.assertEqual(subprocess.run(["flock", "-n", str(lock), "true"]).returncode, 0)

    def test_dismissal_either_step_and_client_failure_never_reboot(self):
        for actions, failure in (
            (["cancel"], "0"), (["2"], "0"),
            (["default", "cancel"], "0"), (["default", "2"], "0"),
            (["default"], "1"),
        ):
            with self.subTest(actions=actions, failure=failure):
                Path(self.env["CALL_LOG"]).unlink(missing_ok=True)
                result = self.run_notifier(ACTIONS=json.dumps(actions), CLIENT_FAIL=failure)
                self.assertEqual(result.returncode, int(failure), result.stderr)
                self.assertFalse(Path(self.env["REBOOT_LOG"]).exists())
                self.assertTrue(self.marker.exists())

    def test_active_update_blocks_confirmed_reboot_and_reports_the_reason(self):
        lock = self.home / ".local/state/smplos/update/update.lock"
        lock.parent.mkdir()
        with lock.open("a") as stream:
            fcntl.flock(stream, fcntl.LOCK_EX | fcntl.LOCK_NB)
            result = self.run_notifier()
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("system update is running", result.stderr)
            self.assertIn("Restart not started", Path(self.env["CALL_LOG"]).read_text())
            self.assertFalse(Path(self.env["REBOOT_LOG"]).exists())

    def test_refused_reboot_is_visible_and_does_not_clear_the_marker(self):
        result = self.run_notifier(REBOOT_FAIL="1")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("restart was refused", result.stderr)
        self.assertIn("Restart not started", Path(self.env["CALL_LOG"]).read_text())
        self.assertTrue(self.marker.exists())
        self.assertEqual(subprocess.run([
            "flock", "-n", str(self.home / ".local/state/smplos/update/update.lock"), "true",
        ]).returncode, 0)

    def test_real_dunst_default_actions_on_private_bus_never_touch_real_systemctl(self):
        tools = ("dbus-run-session", "xvfb-run", "dunst", "dunstctl", "dunstify", "gdbus")
        if not all(shutil.which(tool) for tool in tools):
            self.skipTest("Isolated Dunst/Xvfb action test dependencies are missing")
        self.command("dunstify", f"""#!{sys.executable}
import os, pathlib, subprocess, sys
client = subprocess.Popen(
    ["stdbuf", "-oL", "/usr/bin/dunstify", "--print-id", *sys.argv[1:]],
    stdout=subprocess.PIPE, text=True)
notification_id = client.stdout.readline().strip()
assert notification_id.isdigit(), notification_id
with pathlib.Path(os.environ["CALL_LOG"]).open("a") as stream:
    stream.write(" ".join(sys.argv[1:]) + "\\n")
sys.stdout.write(client.stdout.read())
sys.exit(client.wait())
""")
        probe = r"""
import os, pathlib, signal, subprocess, sys, time
def wait_for(check):
    deadline = time.monotonic() + 8
    while time.monotonic() < deadline:
        if check():
            return
        time.sleep(0.02)
    raise RuntimeError("Isolated notification action did not become ready")
def bus_ready():
    return "true" in subprocess.run(
        ["gdbus", "call", "--session", "--dest", "org.freedesktop.DBus",
         "--object-path", "/org/freedesktop/DBus", "--method",
         "org.freedesktop.DBus.NameHasOwner", "org.freedesktop.Notifications"],
        capture_output=True, text=True, check=True).stdout
daemon = subprocess.Popen(["dunst", "-config", sys.argv[2]],
                          stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
notifier = None
try:
    wait_for(bus_ready)
    notifier = subprocess.Popen(["bash", sys.argv[1]],
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
                               start_new_session=True)
    log = pathlib.Path(os.environ["CALL_LOG"])
    wait_for(lambda: log.exists() and "smplOS: Reboot required" in log.read_text())
    subprocess.run(["dunstctl", "action", "0"], check=True)
    wait_for(lambda: "Restart smplOS now?" in log.read_text())
    assert not pathlib.Path(os.environ["REBOOT_LOG"]).exists()
    subprocess.run(["dunstctl", "action", "0"], check=True)
    out, err = notifier.communicate(timeout=8)
    assert notifier.returncode == 0, out + err
    assert pathlib.Path(os.environ["REBOOT_LOG"]).read_text() == "reboot\n"
finally:
    if notifier is not None and notifier.poll() is None:
        os.killpg(notifier.pid, signal.SIGTERM)
        notifier.communicate(timeout=5)
    daemon.terminate()
    daemon.wait(timeout=5)
"""
        result = subprocess.run(
            ["dbus-run-session", "--", "xvfb-run", "-a", sys.executable, "-c", probe,
             str(NOTIFIER), str(ROOT / "src/shared/configs/dunst/dunstrc")],
            env={**self.env, "WAYLAND_DISPLAY": ""}, capture_output=True, text=True, timeout=35,
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
