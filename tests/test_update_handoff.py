import hashlib
import importlib.machinery
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import time
import unittest
from unittest import mock


ROOT = Path(__file__).resolve().parents[1]
BIN = ROOT / "src/shared/bin"
loader = importlib.machinery.SourceFileLoader("update_handoff", str(BIN / "smplos-update-handoff"))
spec = importlib.util.spec_from_loader(loader.name, loader)
handoff = importlib.util.module_from_spec(spec)
loader.exec_module(handoff)


def generated(source):
    body = re.search(r"cat >> \"\$UPDATE_SCRIPT\" << 'EOF'\n(.*?)\nEOF\n", source, re.S)
    return '#!/bin/bash\nUPDATE_MODE="full"\n' + body[1] + "\n"


class HandoffContracts(unittest.TestCase):
    def test_known_processes_match_real_historical_generated_scripts(self):
        policy = (ROOT / "src/shared/update-policy/legacy-update-runners.sha256").read_text()
        for line in policy.splitlines():
            if not line or line.startswith("#"):
                continue
            digest, commit = line.split()
            with self.subTest(commit=commit):
                source = subprocess.check_output(
                    ["git", "show", f"{commit}:src/shared/bin/smplos-update"], cwd=ROOT, text=True)
                self.assertEqual(hashlib.sha256(generated(source).encode()).hexdigest(), digest)

    def test_a_filename_is_not_authority_to_stop_a_process(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "smplos-update.unrelated.sh"
            path.write_text('echo ready\nread -r _\n')
            child = subprocess.Popen(["bash", str(path)], stdin=subprocess.PIPE, stdout=subprocess.PIPE)
            try:
                self.assertEqual(child.stdout.readline(), b"ready\n")
                with self.assertRaisesRegex(RuntimeError, "Unrecognized legacy"):
                    handoff.legacy_ancestors(child.pid, {"not-this-script"})
                self.assertIsNone(child.poll())
            finally:
                child.communicate(b"\n", timeout=5)

    def test_a_reused_pid_is_not_mistaken_for_the_old_runner(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "handoff"
            old = handoff.process(os.getpid())
            old["start"] = str(int(old["start"]) + 1)
            path.write_text(json.dumps({"legacy": [old]}) + "\n")
            handoff.wait_for_legacy(path)
            self.assertIn("coordinator-ready", path.read_text())

    def test_a_live_legacy_process_blocks_new_package_work(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "handoff"
            path.write_text(json.dumps({"legacy": [handoff.process(os.getpid())]}) + "\n")
            with mock.patch.object(handoff.time, "monotonic", side_effect=[0, 16]):
                with self.assertRaisesRegex(RuntimeError, "old updater is still running"):
                    handoff.wait_for_legacy(path)
            self.assertNotIn("coordinator-ready", path.read_text())

    def test_a_shell_forwarding_an_argument_is_not_the_legacy_runner(self):
        with tempfile.TemporaryDirectory() as directory:
            argument = Path(directory) / "smplos-update.argument.sh"
            argument.write_text("this is not the loaded script\n")
            script = Path(directory) / "terminal"
            script.write_text("echo ready\nread -r _\n")
            child = subprocess.Popen(["bash", str(script), str(argument)],
                                     stdin=subprocess.PIPE, stdout=subprocess.PIPE)
            try:
                self.assertEqual(child.stdout.readline(), b"ready\n")
                self.assertEqual(handoff.legacy_ancestors(child.pid, set()), [])
                self.assertIsNone(child.poll())
            finally:
                child.communicate(b"\n", timeout=5)

    def test_pid_identity_change_never_signals_a_different_process(self):
        child = subprocess.Popen(["bash", "-c", "echo ready; read -r _"],
                                 stdin=subprocess.PIPE, stdout=subprocess.PIPE)
        try:
            self.assertEqual(child.stdout.readline(), b"ready\n")
            item = handoff.process(child.pid)
            item["start"] = "wrong"
            with self.assertRaisesRegex(RuntimeError, "identity changed"):
                handoff.stop_legacy([item])
            self.assertIsNone(child.poll())
        finally:
            child.communicate(b"\n", timeout=5)


class LegacyButtonTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.base = Path(self.temp.name)
        self.system = self.base / "system"
        self.bin = self.system / "usr/local/bin"
        self.bin.mkdir(parents=True)
        self.home = self.base / "home"
        self.repo = self.home / ".local/share/smplos/repo"
        self.upstream = self.base / "upstream"
        self.calls = self.base / "calls"
        self.tty_log = self.base / "terminal.log"
        self.done = self.base / "done"
        self.tmp = self.base / "tmp"
        self.tmp.mkdir()
        self.git = shutil.which("git")
        self.env = {
            **os.environ, "HOME": str(self.home), "PATH": f"{self.bin}:/usr/bin:/bin",
            "SMPLOS_PATH": str(self.repo.parent), "SMPLOS_TERMINAL": str(self.bin / "terminal"),
            "TMPDIR": str(self.tmp), "CALLS": str(self.calls), "TTY_LOG": str(self.tty_log),
            "DONE": str(self.done), "FIXTURE_ROOT": str(self.base),
            "GIT_CONFIG_COUNT": "1", "GIT_CONFIG_KEY_0": "url." + str(self.upstream) + ".insteadOf",
            "GIT_CONFIG_VALUE_0": "https://github.com/smpl-os/smplos.git",
        }
        for name in list(self.env):
            if name.startswith(("SMPLOS_SUDO_", "SMPLOS_UPDATE_", "SMPLOS_UPDATER_")):
                del self.env[name]
        self.command("init", "-q", "--initial-branch=main", str(self.upstream))
        self.command("-C", str(self.upstream), "config", "user.name", "KonTy")
        self.command("-C", str(self.upstream), "config", "user.email", "fixture@example.invalid")
        self.mock("sudo", """#!/usr/bin/python3
import os, subprocess, sys
args = sys.argv[1:]
with open(os.environ["CALLS"], "a") as stream:
    stream.write("sudo " + " ".join(args) + "\\n")
if args == ["-v"]:
    sys.exit(0)
if args and args[0] == "-n":
    args.pop(0)
assert args[0] in ("mkdir", "mktemp", "install", "mv", "rm"), args
assert all(not arg.startswith("/") or arg.startswith(os.environ["FIXTURE_ROOT"] + "/")
           for arg in args), args
sys.exit(subprocess.run(args).returncode)
""")
        self.mock("git", f'''#!/bin/bash
echo "git $*" >> "$CALLS"
if [[ "${{3:-}} ${{4:-}} ${{5:-}}" == "remote get-url origin" ]]; then
    exec "{self.git}" -C "$2" config --get remote.origin.url
fi
if [[ "${{3:-}} ${{4:-}}" == "fetch origin" && "${{FAIL_FETCH:-0}}" == 1 ]]; then exit 42; fi
exec "{self.git}" "$@"
''')
        self.mock("terminal", """#!/usr/bin/python3
import errno, fcntl, os, pty, select, subprocess, sys, termios, time
args = sys.argv[1:]
assert args.pop(0) == "-e"
if os.environ.get("FAIL_TERMINAL") == "1" and any("/repo/src/" in a for a in args):
    sys.exit(23)
if os.environ.get("DAEMON_TERMINAL") == "1" and any("/repo/src/" in a for a in args):
    if os.fork():
        sys.exit(0)
    os.setsid()
    time.sleep(0.2)
master, slave = pty.openpty()
def claim_terminal():
    os.setsid()
    fcntl.ioctl(0, termios.TIOCSCTTY, 0)
child = subprocess.Popen(args, stdin=slave, stdout=slave, stderr=slave, preexec_fn=claim_terminal)
os.close(slave)
os.write(master, b"\\n")
with open(os.environ["TTY_LOG"], "ab", buffering=0) as output:
    while True:
        ready, _, _ = select.select([master], [], [], 0.1)
        if ready:
            try:
                data = os.read(master, 65536)
            except OSError as error:
                if error.errno != errno.EIO: raise
                break
            if not data: break
            output.write(data)
        elif child.poll() is not None:
            break
os.close(master)
code = child.wait()
sys.exit(code if code >= 0 else 128 - code)
""")
        self.mock("notify-send", '#!/bin/bash\necho "notify $*" >> "$CALLS"\n')
        for name in ("pacman", "yay", "paru", "pkexec", "flatpak", "hyprctl"):
            self.mock(name, '#!/bin/bash\necho "FORBIDDEN $0 $*" >> "$CALLS"\nexit 99\n')

    def command(self, *args):
        return subprocess.run([self.git, *args], env=self.env, capture_output=True,
                              text=True, check=True).stdout

    def mock(self, name, source):
        path = self.bin / name
        path.write_text(source)
        path.chmod(0o755)

    def relocate(self, source):
        return source.replace("/usr/local", str(self.system / "usr/local"))

    def prepare(self, commit):
        for name in ("smplos-update", "smplos-os-update"):
            source = subprocess.check_output(
                [self.git, "show", f"{commit}:src/shared/bin/{name}"], cwd=ROOT, text=True)
            source = self.relocate(source)
            source = source.replace('exec pkexec bash "$SELF" "$@"', 'echo legacy-polkit >> "$CALLS"')
            source = source.replace("mktemp /tmp/smplos-update.", 'mktemp "$TMPDIR"/smplos-update.')
            self.mock(name, source)
            path = self.upstream / "src/shared/bin" / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(source)
        legacy = generated((self.bin / "smplos-update").read_text())
        self.command("-C", str(self.upstream), "add", ".")
        self.command("-C", str(self.upstream), "commit", "-qm", "Fixture installed legacy updater")
        self.repo.parent.mkdir(parents=True)
        self.command("clone", "-q", "https://github.com/smpl-os/smplos.git", str(self.repo))
        for directory in ("shared/lib", "shared/update-policy"):
            target = self.upstream / "src" / directory
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copytree(ROOT / "src" / directory, target)
        with (self.upstream / "src/shared/update-policy/legacy-update-runners.sha256").open("a") as stream:
            stream.write(hashlib.sha256(legacy.encode()).hexdigest() + " relocated-test-fixture\n")
        for name in ("smplos-update", "smplos-os-update", "smplos-update-check",
                     "smplos-update-bootstrap", "smplos-update-handoff", "smplos-migrate"):
            source = self.relocate((BIN / name).read_text())
            if name == "smplos-update":
                source = source.split("UPDATE_SCRIPT=$(mktemp", 1)[0]
                source = re.sub(r"check_update_power\(\) \{\n.*?^\}",
                                'check_update_power() { return "${POWER_FAIL:-0}"; }',
                                source, count=1, flags=re.S | re.M)
                source += '\necho FRESH-WRAPPER >> "$CALLS"\ntouch "$DONE"\n'
            elif name == "smplos-update-check":
                source = source.replace('root=Path("/")', f'root=Path("{self.system}")')
            (self.upstream / "src/shared/bin" / name).write_text(source)
        (self.upstream / "src/shared/bin/rebuild-app-cache").write_text("#!/bin/bash\nexit 0\n")
        for name in ("fetch-org.sh", "fetch-apps.sh"):
            (self.upstream / "src" / name).write_text((ROOT / "src" / name).read_text())
        self.command("-C", str(self.upstream), "add", ".")
        self.command("-C", str(self.upstream), "commit", "-qm", "Fixture automatic handoff")

    def click(self, **env):
        result = subprocess.run(["smplos-update", "--mode", "full"], env={**self.env, **env},
                                capture_output=True, text=True, timeout=20)
        deadline = time.monotonic() + 15
        while time.monotonic() < deadline:
            calls = self.calls.read_text() if self.calls.exists() else ""
            if self.done.exists() or "notify Update OS could not start" in calls:
                break
            if env.get("POWER_FAIL") == "1" and self.tty_log.exists() and "Plug in power" in self.tty_log.read_text():
                break
            time.sleep(0.05)
        return result

    def assert_no_legacy_package_work(self):
        calls = self.calls.read_text()
        self.assertNotIn("FORBIDDEN", calls)
        for command in ("pacman ", "yay ", "paru "):
            self.assertNotIn(command, calls)

    def test_installed_legacy_button_reopens_new_coordinator_without_a_manual_command(self):
        self.prepare("e4188ff")
        self.click()
        self.assertTrue(self.done.exists(), self.tty_log.read_text())
        self.assert_no_legacy_package_work()
        self.assertEqual(self.calls.read_text().splitlines().count("legacy-polkit"), 1)
        self.assertEqual(self.calls.read_text().splitlines().count("sudo -v"), 1)

    def test_broken_historical_self_reexec_is_caught_by_the_fetched_app_script(self):
        self.prepare("0ee677f")
        self.click()
        self.assertTrue(self.done.exists(), self.tty_log.read_text())
        self.assert_no_legacy_package_work()

    def test_first_git_updater_is_caught_by_the_refreshed_migration_runner(self):
        self.prepare("c14dcd6")
        self.click()
        self.assertTrue(self.done.exists(), self.tty_log.read_text())
        self.assert_no_legacy_package_work()

    def test_no_ac_stops_automatically_before_new_auth_or_legacy_package_work(self):
        self.prepare("e4188ff")
        self.click(POWER_FAIL="1")
        self.assertFalse(self.done.exists())
        self.assertIn("Plug in power", self.tty_log.read_text())
        self.assertNotIn("sudo -v", self.calls.read_text())
        self.assert_no_legacy_package_work()

    def test_failed_replacement_fetch_does_not_return_to_the_old_package_sequence(self):
        self.prepare("e4188ff")
        self.click(FAIL_FETCH="1")
        self.assertFalse(self.done.exists())
        self.assertIn("notify Update OS could not start", self.calls.read_text())
        self.assert_no_legacy_package_work()

    def test_failed_new_terminal_does_not_resume_the_old_update(self):
        self.prepare("e4188ff")
        self.click(FAIL_TERMINAL="1")
        self.assertFalse(self.done.exists())
        self.assertIn("notify Update OS could not start", self.calls.read_text())
        self.assert_no_legacy_package_work()

    def test_an_inherited_coordinator_flag_without_its_lock_does_not_skip_handoff(self):
        self.prepare("e4188ff")
        self.click(SMPLOS_UPDATE_COORDINATOR="1")
        self.assertTrue(self.done.exists(), self.tty_log.read_text())
        self.assert_no_legacy_package_work()

    def test_a_daemonized_terminal_can_open_after_its_launcher_exits_successfully(self):
        self.prepare("e4188ff")
        self.click(DAEMON_TERMINAL="1")
        self.assertTrue(self.done.exists(), self.tty_log.read_text())
        self.assert_no_legacy_package_work()

    def test_a_subsequent_button_click_uses_the_installed_coordinator_directly(self):
        self.prepare("e4188ff")
        self.click()
        self.assertTrue(self.done.exists(), self.tty_log.read_text())
        self.done.unlink()
        self.click()
        self.assertTrue(self.done.exists(), self.tty_log.read_text())
        self.assertEqual(self.calls.read_text().splitlines().count("legacy-polkit"), 1)
        self.assertEqual(self.calls.read_text().splitlines().count("sudo -v"), 2)
        self.assert_no_legacy_package_work()

    def test_a_current_installed_wrapper_automatically_refreshes_a_legacy_checkout(self):
        self.prepare("e4188ff")
        for name in ("smplos-update", "smplos-update-bootstrap"):
            shutil.copyfile(self.upstream / "src/shared/bin" / name, self.bin / name)
        self.click()
        self.assertTrue(self.done.exists(), self.tty_log.read_text())
        self.assertNotIn("legacy-polkit", self.calls.read_text())
        self.assertEqual(self.calls.read_text().splitlines().count("sudo -v"), 1)
        self.assert_no_legacy_package_work()


if __name__ == "__main__":
    unittest.main()
