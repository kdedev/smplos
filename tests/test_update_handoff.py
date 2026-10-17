import hashlib
import importlib.machinery
import importlib.util
import io
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys
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

    def test_handoff_keeps_the_log_descriptor_instead_of_recreating_a_user_owned_tmp_file(self):
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory)
            bootstrap = base / "src/shared/bin/smplos-update-bootstrap"
            bootstrap.parent.mkdir(parents=True)
            bootstrap.write_text("fixture\n")
            original_open = Path.open

            def protected_open(path, mode="r", *args, **kwargs):
                if path.parent == base and path.suffix == ".log" and any(flag in mode for flag in "wa"):
                    raise PermissionError("fs.protected_regular fixture")
                return original_open(path, mode, *args, **kwargs)

            def replacement(*args, **kwargs):
                record = next(base.glob("*.log"))
                with open(record, "a") as stream:
                    stream.write("\ncoordinator-ready\n")
                return mock.Mock()

            with mock.patch.object(handoff.tempfile, "tempdir", directory), \
                    mock.patch.object(Path, "open", protected_open), \
                    mock.patch.object(handoff.os, "dup2"), \
                    mock.patch.object(handoff.subprocess, "Popen", side_effect=replacement), \
                    mock.patch("sys.stdout", new_callable=io.StringIO):
                self.assertEqual(handoff.launch(base, [], os.getuid()), 75)


@unittest.skipUnless(os.geteuid() == 0, "Requires the separate isolated sudo CI step")
class PrivilegedHandoffTests(unittest.TestCase):
    def setUp(self):
        self.uid = int(os.environ.get("SUDO_UID", "0"))
        if not self.uid:
            self.skipTest("Run via sudo as a non-root desktop/test user")

    def test_linux_rejects_the_previous_root_reopen_after_chown(self):
        if Path("/proc/sys/fs/protected_regular").read_text().strip() == "0":
            self.skipTest("This host has disabled the protected_regular kernel safeguard")
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory)
            base.chmod(0o1777)
            record = base / "user-owned.log"
            record.write_text("original record\n")
            os.chown(record, self.uid, -1)
            with self.assertRaises(PermissionError):
                record.open("a")
            self.assertEqual(record.read_text(), "original record\n")

    def test_real_root_to_user_handoff_works_with_protected_tmp_logs(self):
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory)
            base.chmod(0o755)
            home = base / "home"
            home.mkdir()
            os.chown(home, self.uid, -1)
            state = home / ".local/state/smplos/update"
            state.mkdir(parents=True)
            state.chmod(0o700)
            versions = home / ".local/state/smplos/app-versions"
            versions.mkdir()
            marker = versions / "smpl-apps"
            marker.write_text("old native version\n")
            untouched = state / "old-update.log"
            untouched.write_text("preserve the old root log\n")
            bootstrap = base / "src/shared/bin/smplos-update-bootstrap"
            bootstrap.parent.mkdir(parents=True)
            bootstrap.write_text(
                '#!/bin/bash\nset -e\nprintf "replacement uid=%s\\n" "$(id -u)"\n'
                f'echo new > {shlex.quote(str(state / "probe"))}\n'
                f'echo new > {shlex.quote(str(marker))}\n'
                'printf "\\ncoordinator-ready\\n" >> "$4"\n')
            result = subprocess.run(
                [sys.executable, "-c",
                 "import os,pwd,runpy,sys; from pathlib import Path; "
                 "module=runpy.run_path(sys.argv[1]); "
                 "module['launch'].__globals__['user_environment'] = "
                 "lambda uid: (pwd.getpwuid(uid).pw_name, dict(os.environ,HOME=sys.argv[2]+'/home')); "
                 "sys.exit(module['launch'](Path(sys.argv[2]), [], int(sys.argv[3])))",
                 str(BIN / "smplos-update-handoff"), str(base), str(self.uid)],
                capture_output=True, text=True, timeout=40)
            match = re.search(r"Details: (/.*\.log)", result.stdout)
            self.assertIsNotNone(match, result.stdout + result.stderr)
            record = Path(match[1])
            self.addCleanup(record.unlink)
            self.assertEqual(result.returncode, 75, record.read_text() + result.stderr)
            self.assertEqual(record.stat().st_uid, self.uid)
            self.assertIn(f"replacement uid={self.uid}", record.read_text())
            self.assertIn("coordinator-ready", record.read_text())
            self.assertEqual(state.stat().st_uid, self.uid)
            self.assertEqual(marker.stat().st_uid, self.uid)
            self.assertEqual((state / "probe").stat().st_uid, self.uid)
            self.assertEqual(untouched.stat().st_uid, 0)
            self.assertEqual(untouched.read_text(), "preserve the old root log\n")

    def test_state_ownership_repair_does_not_follow_links_outside_the_desktop_home(self):
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory)
            home = base / "home"
            home.mkdir()
            os.chown(home, self.uid, -1)
            outside = base / "outside"
            outside.mkdir()
            (home / ".local").symlink_to(outside, target_is_directory=True)
            with self.assertRaises(OSError):
                handoff.restore_state_ownership(home, self.uid)
            self.assertEqual(outside.stat().st_uid, 0)

    def test_state_ownership_repair_refuses_hardlinked_version_markers(self):
        with tempfile.TemporaryDirectory() as directory:
            home = Path(directory) / "home"
            home.mkdir()
            os.chown(home, self.uid, -1)
            versions = home / ".local/state/smplos/app-versions"
            versions.mkdir(parents=True)
            outside = Path(directory) / "outside"
            outside.write_text("keep ownership and contents\n")
            (versions / "smpl-apps").hardlink_to(outside)
            with self.assertRaisesRegex(RuntimeError, "Unexpected state file"):
                handoff.restore_state_ownership(home, self.uid)
            self.assertEqual(outside.stat().st_uid, 0)
            self.assertEqual(outside.read_text(), "keep ownership and contents\n")


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
