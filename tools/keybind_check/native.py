"""Per-bind verification by Hyprland's own config parser (`Hyprland --verify-config`).

Verification needs no GPU, display or seat: Hyprland loads a Lua config, reports every error
the config raised and exits 1. Each bind goes through the real bindings_loader.lua in a
config of its own, so Hyprland's key and modifier parser, the hl.dsp constructors and the
loader's translation are exercised together, and a failure names the one bind behind it.

Hyprland only reads the arguments of `hl.dsp.*` constructors when a dispatcher runs, so a
misspelled option value (`float({action='togle'})`) passes here; the lab catches those.
"""
from __future__ import annotations

import concurrent.futures
import dataclasses
import itertools
import os
import re
import shutil
import subprocess
import tempfile
from pathlib import Path

from . import model, rules

BINARY_ENV = "HYPRLAND_BIN"
VERIFY_LUA = model.ROOT / "tools/keybind_check/verify.lua"
TIMEOUT = 30
# Hyprland refuses to load a config as root. Verifying only parses it and exits, never starting
# a session, so the check is safe and lets a root container (CI) run it.
ROOT_OVERRIDE = "--i-am-really-stupid"
RESULT_HEADER = "Config parsing result:"
# Variables that could point a verification at a running session or a user's settings.
_SESSION_ENV = re.compile(r"^(HYPR|WAYLAND|XDG_|DISPLAY$|KB_)")
# Errors raised from C functions carry this meaningless position prefix.
_C_POSITION = re.compile(r"^=\[C\]:-1:\s*")


class Unavailable(RuntimeError):
    """Hyprland is not installed, so nothing can be verified natively."""


def binary():
    """The Hyprland executable used for verification, or None."""
    return os.environ.get(BINARY_ENV) or shutil.which("Hyprland")


def available():
    return binary() is not None


def bind_text(bind):
    """The bind as a stand-alone file, wrapped in its submap when it belongs to one."""
    if bind.submap:
        return f"submap = {bind.submap}\n{bind.raw}\nsubmap = reset\n"
    return bind.raw + "\n"


def dispatch_expressions(call):
    """Lua expressions an exec bind hands to `hyprctl dispatch`."""
    if call.name != "exec_cmd" or not call.args or not isinstance(call.args[0], str):
        return []
    try:
        return [text for verb, text in rules.hyprctl_expressions(call.args[0]) if verb == "dispatch"]
    except ValueError:
        return []  # the static rules report commands that do not parse


@dataclasses.dataclass(frozen=True)
class Verdict:
    ok: bool
    message: str = ""


class Verifier:
    """Runs `Hyprland --verify-config` for single binds, dispatch expressions and Lua files."""

    def __init__(self, hyprland=None, jobs=None):
        self.hyprland = hyprland or binary()
        if not self.hyprland:
            raise Unavailable(f"Hyprland is not installed (set {BINARY_ENV} to its path)")
        self.jobs = jobs or min(8, os.cpu_count() or 1)
        self._tmp = tempfile.TemporaryDirectory(prefix="keybind-check-")
        root = Path(self._tmp.name)
        self._home, self._run = root / "home", root / "run"
        self._home.mkdir()
        self._run.mkdir(mode=0o700)
        self._numbers = itertools.count()
        self._env = {key: value for key, value in os.environ.items()
                     if not _SESSION_ENV.match(key)}
        self._env.update(HOME=str(self._home), XDG_RUNTIME_DIR=str(self._run),
                         HYPRLAND_NO_CRASHREPORTER="1")

    def close(self):
        self._tmp.cleanup()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()

    def version(self):
        result = subprocess.run([self.hyprland, "--version"], env=self._env, timeout=TIMEOUT,
                                capture_output=True, text=True)
        return (result.stdout.splitlines() or [""])[0]

    def _scrub(self, text):
        text = text.split("stack traceback:")[0]
        text = text.replace(f"{model.ROOT}/", "").replace(self._tmp.name, "<tmp>")
        lines = (_C_POSITION.sub("", line.strip()) for line in text.splitlines())
        return " | ".join(line for line in lines if line)

    def _verify(self, mode, **variables):
        command = [self.hyprland, "--verify-config", "--config", str(VERIFY_LUA)]
        if os.geteuid() == 0:
            command.append(ROOT_OVERRIDE)
        env = dict(self._env, KB_MODE=mode, **variables)
        try:
            result = subprocess.run(command, env=env, capture_output=True, timeout=TIMEOUT)
        except subprocess.TimeoutExpired:
            return Verdict(False, f"Hyprland did not finish within {TIMEOUT}s")
        output = (result.stdout + result.stderr).decode("utf-8", "replace")
        _, found, report = output.partition(RESULT_HEADER)
        report = self._scrub(report if found else output[-600:])
        # Text the config printed after "config ok" (the loader notes unmapped dispatchers
        # that way) is not a verification failure.
        parsed = found and report.startswith("config ok")
        if result.returncode == 0 and parsed:
            return Verdict(True)
        if parsed:
            report = f"Hyprland exited {result.returncode} after reporting 'config ok'"
        elif not found:
            report = f"Hyprland exited {result.returncode} without a parse result: {report}"
        return Verdict(False, report)

    def bind(self, bind, policy=False):
        """Verdict for one bind line, loaded through the real bindings_loader.lua."""
        path = self._home / f"bind{next(self._numbers)}.conf"
        path.write_text(bind_text(bind), encoding="utf-8")
        return self._verify("bind", KB_LOADER=str(model.LOADER), KB_CONF=str(path),
                            KB_POLICY="1" if policy else "0")

    def expression(self, expression):
        """Verdict for building the dispatcher a `hyprctl dispatch "<expression>"` runs."""
        return self._verify("expression", KB_EXPR=expression)

    def lua_file(self, path):
        """Verdict for running a Lua bind module such as popup_watchers.lua."""
        return self._verify("file", KB_FILE=str(path))

    def check(self, entries, policies=(False, True)):
        """Findings for every entry's bind line (per policy) and dispatch expressions."""
        jobs = []
        for entry in entries:
            for policy in policies:
                jobs.append((entry, "native", lambda p=policy, b=entry.bind: self.bind(b, p), policy))
            for text in dispatch_expressions(entry.call):
                jobs.append((entry, "native-expression", lambda t=text: self.expression(t), None))
        with concurrent.futures.ThreadPoolExecutor(self.jobs) as pool:
            verdicts = list(pool.map(lambda job: job[2](), jobs))
        return _findings(jobs, verdicts, len(policies))

    def check_file(self, path):
        """Findings for a Lua bind module, or none when Hyprland accepts it."""
        verdict = self.lua_file(path)
        where = model.display_path(path)
        return [] if verdict.ok else [rules.finding("native", where, verdict.message)]


def _findings(jobs, verdicts, policy_count):
    """One finding per failure; a failure shared by every policy mode is reported once."""
    failures = {}
    for (entry, rule, _, policy), verdict in zip(jobs, verdicts):
        if not verdict.ok:
            failures.setdefault((id(entry), rule, verdict.message), []).append((entry, policy))
    findings = []
    for (_, rule, message), hits in failures.items():
        entry = hits[0][0]
        policies = {policy for _, policy in hits}
        if policies != {None} and len(policies) < policy_count:
            message += f" (workspace policy {'on' if True in policies else 'off'})"
        findings.append(rules.finding(rule, entry.bind.where, message, entry.bind.label))
    return findings
