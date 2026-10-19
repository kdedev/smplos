"""Press every bind in a throwaway headless Hyprland and check what the compositor does.

This is the only check that types a chord the way a keyboard would, so it catches what
a parser cannot: a chord another bind shadows, a bind that fires twice, a dispatcher that
errors at run time and a dispatcher that runs but does the wrong thing.

It is local only. Hyprland needs a DRM render node even without a display, and CI
runners have none. The lab is built so that it cannot touch the session it runs in:

  * its own XDG_RUNTIME_DIR and HOME, so hyprctl and Wayland clients cannot reach the
    real compositor;
  * /dev/input is hidden in a user namespace, so real keystrokes never arrive;
  * PATH holds only stub programs, so a bind runs nothing real (an exec bind is judged by
    a call log) and binds that could write files or signal processes are skipped;
  * the only process it ever signals is the compositor it started.
"""
from __future__ import annotations

import dataclasses
import glob
import json
import os
import re
import shlex
import shutil
import signal
import subprocess
import sys
import tempfile
import time
from pathlib import Path

from . import model, native, rules, vkbd, xkb

WINDOW_COUNT = 2
START_TIMEOUT = 30.0
TRIGGER_TIMEOUT = 2.0
SETTLE = 0.2
SOCKET_LIMIT = 107  # sun_path
SIGNATURE_LENGTH = 64  # commit hash, start time and a random suffix
GPU_ENV = ("AQ_DRM_DEVICES", "AQ_BACKENDS", "GBM_BACKEND", "LD_LIBRARY_PATH", "LIBGL_DRIVERS_PATH",
           "MESA_LOADER_DRIVER_OVERRIDE", "__GLX_VENDOR_LIBRARY_NAME", "__EGL_VENDOR_LIBRARY_FILENAMES")

PASS, FAIL, SKIP = "pass", "fail", "skip"

# Run inside `unshare --user --map-root-user --mount`: hide the keyboards, then drop root again.
NAMESPACE = (
    'if [ -d /dev/input ] && ! mount --bind "$1" /dev/input; then '
    'echo "lab: cannot hide /dev/input, refusing to start" >&2; exit 97; fi; '
    'unshare=$2; uid=$3; gid=$4; shift 4; '
    'exec "$unshare" --user --map-user="$uid" --map-group="$gid" -- "$@"'
)
WINDOW_IMPORT = 'import gi; gi.require_version("Gtk", "3.0"); from gi.repository import Gtk'
WINDOW_SCRIPT = WINDOW_IMPORT + """
import sys
window = Gtk.Window(title=sys.argv[1])
window.set_default_size(480, 320)
window.connect("destroy", Gtk.main_quit)
window.show_all()
Gtk.main()
"""

STUB = """#!/bin/sh
line=@NAME@
for arg in "$@"; do line="$line $arg"; done
printf '%s\\n' "$line" >> @CALLS@
exit @RC@
"""
# Forwards `hyprctl dispatch` to the lab compositor only; anything else is logged and ignored.
HYPRCTL_STUB = """#!/bin/sh
printf 'hyprctl %s\\n' "$*" >> @CALLS@
[ "$1" = dispatch ] || exit 0
XDG_RUNTIME_DIR=@RUN@
export XDG_RUNTIME_DIR
for dir in @RUN@/hypr/*; do HYPRLAND_INSTANCE_SIGNATURE=${dir##*/}; done
export HYPRLAND_INSTANCE_SIGNATURE
out=$(@REAL@ "$@" 2>&1)
rc=$?
nl='
'
printf 'result %s %s\\n' "$rc" "${out%%"$nl"*}" >> @CALLS@
printf '%s\\n' "$out"
exit "$rc"
"""
FAILING_STUBS = {"pkill": 1, "pgrep": 1}  # so `pkill x || start x` takes the start branch

HYPRLAND_LUA = """\
package.path = @HYPR@ .. "/?.lua;" .. package.path
hl.config({ debug = { disable_logs = false, enable_stdout_logs = true } })
@MONITORS@
hl.monitor({ output = "HEADLESS-1", mode = "1920x1080@60", position = "0x0", scale = 1 })
-- looknfeel.lua: the scrolling layout is what the layoutmsg binds talk to.
hl.config({
  general = { layout = "scrolling", gaps_in = 5, gaps_out = 10, border_size = 2 },
  scrolling = { fullscreen_on_one_column = false, column_width = 0.95, focus_fit_method = 1, follow_focus = true },
  animations = { enabled = false },
  misc = { disable_hyprland_logo = true, disable_splash_rendering = true },
  input = { kb_layout = "us" },
})
_G.smplos_workspace_policy_active = @POLICY@
local loader = require("bindings_loader")
@LOADS@
"""

ANSI = re.compile(r"\x1b\[[0-9;]*[A-Za-z]")  # Hyprland colours its log levels even when logging to a file
TRIGGER = re.compile(r"Keybind triggered, calling dispatcher \((\d+), [^,]*, (\d+), ")
EXECUTED = re.compile(r"\[executor\] Executing (.*)")
LEVELLED = re.compile(r"^ERR \]: (.*)")
BENIGN = ("setPointerFocus", "setKeyboardFocus", "start-hyprland")  # lab noise: no real seat or launcher

_WORD = re.compile(r"[A-Za-z0-9_][A-Za-z0-9_.+-]*\Z")
_UNSAFE_TEXT = ("`", "$(", "${", "<", ">")  # the shell would act on real files or run real commands
# Builtins run in the shell itself, so a stub cannot observe them (and `kill` could signal real processes).
_BUILTINS = frozenset("""
    : . [ alias bg bind break builtin caller cd command compgen complete continue declare dirs disown
    echo enable eval exec exit export false fc fg getopts hash help history jobs kill let local logout
    mapfile popd printf pushd pwd read readarray readonly return set shift shopt source suspend test
    times trap true type typeset ulimit umask unalias unset wait
""".split())


class LabError(RuntimeError):
    """The lab could not be started, or died while it ran."""


@dataclasses.dataclass(frozen=True)
class Result:
    label: str
    where: str
    status: str
    detail: str = ""


# ── What can run here ───────────────────────────────────────────────────────

def hyprctl_binary():
    hyprland = native.binary()
    sibling = Path(hyprland).with_name("hyprctl") if hyprland else None
    return str(sibling) if sibling and sibling.is_file() else shutil.which("hyprctl")


def scratch_dir():
    # Unix socket paths are short; a long $TMPDIR would push Hyprland's socket over the limit.
    base = "/tmp" if os.access("/tmp", os.W_OK) else tempfile.gettempdir()
    return Path(tempfile.mkdtemp(prefix="kbl-", dir=base))


def namespaced(argv, empty):
    """`argv` run as this user, with /dev/input replaced by the empty directory `empty`."""
    unshare = shutil.which("unshare")
    return [unshare, "--user", "--map-root-user", "--mount", "sh", "-c", NAMESPACE, "lab",
            str(empty), unshare, str(os.getuid()), str(os.getgid()), *argv]


def drm_connectors():
    """Connector names of the real outputs, which the lab turns off."""
    return sorted({re.sub(r"^card\d+-", "", Path(path).name) for path in glob.glob("/sys/class/drm/card*-*")})


def preflight():
    """Reasons the lab cannot run on this machine; empty when it can."""
    problems = []
    if os.geteuid() == 0:
        problems.append("running as root: Hyprland will not start a session as root, "
                        "so run the lab as a regular user")
    if not native.available():
        problems.append(f"Hyprland is not installed (set {native.BINARY_ENV} to its path)")
    elif not hyprctl_binary():
        problems.append("hyprctl is not installed")
    if not xkb.available():
        problems.append("libxkbcommon is missing (needed to type the chords)")
    if not shutil.which("unshare"):
        return problems + ["unshare (util-linux) is missing"]
    nodes = sorted(glob.glob("/dev/dri/renderD*"))
    if not nodes:
        problems.append("no /dev/dri/renderD* node: Hyprland needs a GPU even without a display")
    else:
        scratch = scratch_dir()
        try:
            opened = any(subprocess.run(namespaced(["sh", "-c", ': <> "$1"', "probe", node], scratch),
                                        capture_output=True).returncode == 0 for node in nodes)
        finally:
            shutil.rmtree(scratch, ignore_errors=True)
        if not opened:
            problems.append("no render node can be opened in an unprivileged user namespace")
    if subprocess.run([sys.executable, "-c", WINDOW_IMPORT], capture_output=True).returncode != 0:
        problems.append("PyGObject with GTK 3 is missing (the test windows are GTK windows)")
    return problems


def exec_plan(command):
    """(command names to stub, None) when an exec bind is safe to run here, else (None, why not)."""
    if any(mark in command for mark in _UNSAFE_TEXT):
        return None, "redirects or substitutes, so it is not run"
    try:
        words = rules.command_words(command, paths=True)
    except ValueError as error:
        return None, f"cannot be parsed ({error})"
    for word in words:
        if word in _BUILTINS:
            return None, f"runs {word!r}, a shell builtin the lab cannot observe"
        if not _WORD.match(word):
            return None, f"runs {word!r}, which the lab will not stub"
    for _verb, expression in rules.hyprctl_expressions(command):
        if re.search(r"\bexit\b", expression):
            return None, "would end the compositor"
    return words, None


# ── What the compositor must do for a bind ──────────────────────────────────
# Effects are keyed by the hyprlang dispatcher a bind declares, not by the Lua call the
# loader made from it: they check that the translation kept the bind's meaning.

@dataclasses.dataclass(frozen=True)
class Effect:
    """`stage(lab)` prepares state and returns it; `verify(lab, state)` returns a complaint or None."""
    verify: object
    stage: object = None


NUMBER = re.compile(r"[0-9]+")


def _workspace(bind):
    arg = bind.arg
    if NUMBER.fullmatch(arg):
        want, hops = int(arg), ((2,) if arg == "1" else ())
    elif arg in ("+1", "-1"):
        want, hops = 5 + int(arg), (5,)
    elif arg == "previous":
        want, hops = 3, (3, 5)
    else:
        return None

    def stage(lab):
        for number in hops:
            lab.focus_workspace(number)

    def verify(lab, _):
        if lab.wait(lambda: lab.workspace().get("id") == want):
            return None
        return f"workspace {lab.workspace().get('id')} is active, expected {want}"
    return Effect(verify, stage)


def _move(silent):
    def make(bind):
        arg = bind.arg
        numeric = bool(NUMBER.fullmatch(arg))
        if not numeric and not (silent and arg.startswith("special:")):
            return None
        target, key = (int(arg), "id") if numeric else (arg, "name")

        def stage(lab):
            if target == 1:
                lab.send_active_to(2)
            return lab.active().get("address"), lab.workspace().get("id")

        def verify(lab, state):
            address, start = state

            def where():
                return lab.client(address).get("workspace", {}).get(key)
            if not lab.wait(lambda: where() == target):
                return f"the window is on workspace {where()!r}, expected {target!r}"
            focus, want = lab.workspace().get("id"), (start if silent or not numeric else target)
            if focus != want:
                how = "a silent move must leave focus where it was" if silent else "focus must follow the window"
                return f"workspace {focus} is active afterwards, expected {want}: {how}"
            shown = lab.monitor().get("specialWorkspace", {}).get("name")
            if silent and not numeric and shown:
                return f"{shown} is showing afterwards: a silent move must not reveal it"
            return None
        return Effect(verify, stage)
    return make


def _submap(bind):
    want = "default" if bind.arg == "reset" else bind.arg
    return Effect(lambda lab, _: None if lab.wait(lambda: lab.submap() == want)
                  else f"submap is {lab.submap()!r}, expected {want!r}") if bind.arg else None


def _toggle_floating(_bind):
    return Effect(lambda lab, before: None if lab.wait(lambda: lab.active().get("floating") != before)
                  else "the window did not change between tiled and floating",
                  lambda lab: lab.active().get("floating"))


def _close(_bind):
    return Effect(lambda lab, address: None if lab.wait(lambda: not lab.client(address))
                  else "the window is still open",
                  lambda lab: lab.active().get("address"))


def _fullscreen(bind):
    want = {"0": 2, "1": 1}.get(bind.arg)
    if want is None:
        return None
    name = "full screen" if want == 2 else "maximized"
    return Effect(lambda lab, _: None if lab.wait(lambda: lab.active().get("fullscreen") == want)
                  else f"the window is not {name} (fullscreen={lab.active().get('fullscreen')})")


def _fullscreen_state(bind):
    match = re.fullmatch(r"(-?\d+)\s+(-?\d+)", bind.arg)
    if not match:
        return None
    internal, client = int(match[1]), int(match[2])

    def holds(window):
        return ((internal < 0 or window.get("fullscreen") == internal)
                and (client < 0 or window.get("fullscreenClient") == client))
    return Effect(lambda lab, _: None if lab.wait(lambda: holds(lab.active()))
                  else f"fullscreen={lab.active().get('fullscreen')} client={lab.active().get('fullscreenClient')},"
                       f" expected {internal} and {client}")


def _group(_bind):
    return Effect(lambda lab, _: None if lab.wait(lambda: bool(lab.active().get("grouped")))
                  else "the window did not become a group")


def _special(bind):
    name = f"special:{bind.arg}"
    return Effect(lambda lab, _: None if lab.wait(lambda: lab.monitor().get("specialWorkspace", {}).get("name") == name)
                  else f"{name} is not showing") if bind.arg else None


EFFECTS = {
    "workspace": _workspace, "movetoworkspace": _move(False), "movetoworkspacesilent": _move(True),
    "submap": _submap, "togglefloating": _toggle_floating, "killactive": _close,
    "fullscreen": _fullscreen, "fullscreenstate": _fullscreen_state, "togglegroup": _group,
    "togglespecialworkspace": _special,
}


def effect_for(bind, call):
    if call.name == "exec_cmd" and bind.dispatcher != "exec":
        return None  # the workspace policy handed it to workspace-ctl, which the lab stubs
    make = EFFECTS.get(bind.dispatcher)
    return make(bind) if make else None


# ── Planning one bind ───────────────────────────────────────────────────────

@dataclasses.dataclass
class Plan:
    entry: object
    code: int
    mask: int
    keysym: int
    command: str | None = None
    words: list = dataclasses.field(default_factory=list)
    effect: Effect | None = None


def modifier_mask(bind):
    mask = 0
    for token in model.words(bind.mods):
        bit = vkbd.MOD_BITS.get(model.MOD_NAMES.get(token))
        if bit is None:
            return None
        mask |= bit
    return mask


def make_plan(entry, keymap):
    """(Plan, None), or (None, why this bind is not typed)."""
    bind, call = entry.bind, entry.call
    key = bind.key_name
    if "m" in bind.flags or key.lower().startswith(("mouse:", "switch:")) or key.lower() in model.SPECIAL_KEYS:
        return None, "pointer bind"
    if call.name == "exit":
        return None, "would end the compositor"
    if not call.enabled:
        return None, "disabled"
    mask, keysym = modifier_mask(bind), xkb.canonical(key)
    code = keymap.evdev_code(key) if keysym else None
    if mask is None or code is None:
        return None, f"no way to type {bind.label} on a us keyboard"
    plan = Plan(entry, code, mask, keysym, effect=effect_for(bind, call))
    if call.name == "exec_cmd" and call.args and isinstance(call.args[0], str):
        plan.command = call.args[0]
        plan.words, why = exec_plan(plan.command)
        if why:
            return None, why
    return plan, None


# ── The compositor ──────────────────────────────────────────────────────────

def lua_string(text):
    return '"' + str(text).replace("\\", "\\\\").replace('"', '\\"').replace("\n", "\\n") + '"'


def poll(predicate, timeout, interval=0.02):
    deadline = time.monotonic() + timeout
    while True:
        value = predicate()
        if value or time.monotonic() >= deadline:
            return value
        time.sleep(interval)


@dataclasses.dataclass
class Seen:
    triggers: list
    executed: list
    stubs: list
    results: list
    errors: list


class Lab:
    """A headless Hyprland with two windows and a virtual keyboard, until close()."""

    def __init__(self, sources, policy=False, commands=(), keep=False):
        self.sources, self.policy, self.keep = sources, policy, keep
        self.commands = sorted(set(commands))
        self.process = None
        self.keyboard = None
        self.windows = []
        self.keymap = xkb.Keymap("us")
        self.root = scratch_dir()
        self.run_dir = self.root / "run"
        self.log, self.calls = self.root / "hypr.out", self.root / "calls.log"
        self._signature = None
        self._log_file = None

    # lifecycle

    def start(self):
        try:
            self._prepare()
            self._launch()
            self._wait_ready()
            self.keyboard = vkbd.VirtualKeyboard(self._wayland_socket(), self.keymap.text())
            time.sleep(0.3)
            self.stage()
        except BaseException:
            self.close()
            raise
        return self

    def close(self):
        if self.keyboard:
            try:
                self.keyboard.close()
            except (OSError, vkbd.WaylandError):
                pass
            self.keyboard = None
        self._reap_windows()
        self._stop_compositor()
        if self.keep:
            print(f"lab files kept in {self.root}", file=sys.stderr)
        else:
            shutil.rmtree(self.root, ignore_errors=True)

    def __enter__(self):
        return self.start()

    def __exit__(self, *_):
        self.close()

    def _prepare(self):
        for name in ("run", "home", "stubs", "empty"):
            (self.root / name).mkdir(mode=0o700)
        self.calls.touch()
        stubs, real = self.root / "stubs", hyprctl_binary()
        quoted = {"@CALLS@": shlex.quote(str(self.calls)), "@RUN@": shlex.quote(str(self.run_dir)),
                  "@REAL@": shlex.quote(real)}
        self._write_stub(stubs / "hyprctl", HYPRCTL_STUB, quoted)
        for name in self.commands:
            if name != "hyprctl":
                self._write_stub(stubs / name, STUB, {**quoted, "@NAME@": shlex.quote(name),
                                                      "@RC@": str(FAILING_STUBS.get(name, 0))})
        monitors = "\n".join(f"hl.monitor({{ output = {lua_string(name)}, disabled = true }})"
                             for name in drm_connectors())
        loads = "\n".join(f"loader.load({lua_string(source.path)})" for source in self.sources
                          if Path(source.path).is_file())
        (self.root / "home/hyprland.lua").write_text(
            HYPRLAND_LUA.replace("@HYPR@", lua_string(model.HYPR_DIR)).replace("@MONITORS@", monitors)
            .replace("@POLICY@", "true" if self.policy else "false").replace("@LOADS@", loads),
            encoding="utf-8")

    @staticmethod
    def _write_stub(path, template, values):
        text = template
        for key, value in values.items():
            text = text.replace(key, value)
        path.write_text(text, encoding="utf-8")
        path.chmod(0o755)

    def _launch(self):
        longest = len(os.fsencode(self.run_dir)) + len("/hypr/") + SIGNATURE_LENGTH + len("/.socket.sock")
        if longest >= SOCKET_LIMIT:
            raise LabError(f"{self.run_dir} is too long for Hyprland's socket paths")
        env = {"PATH": str(self.root / "stubs"), "HOME": str(self.root / "home"),
               "XDG_RUNTIME_DIR": str(self.run_dir), "LANG": "C.UTF-8",
               "DBUS_SESSION_BUS_ADDRESS": "unix:path=/nonexistent", "LIBSEAT_BACKEND": "noop",
               "HYPRLAND_NO_CRASHREPORTER": "1", "HYPRLAND_NO_SD_NOTIFY": "1", "HYPRLAND_NO_SD_VARS": "1"}
        env.update({name: os.environ[name] for name in GPU_ENV if name in os.environ})
        command = namespaced([shutil.which("env"), "-i", *(f"{key}={value}" for key, value in env.items()),
                              os.path.realpath(native.binary()), "-c", str(self.root / "home/hyprland.lua")],
                             self.root / "empty")
        self._log_file = open(self.log, "wb")
        self.process = subprocess.Popen(command, stdin=subprocess.DEVNULL, stdout=self._log_file,
                                        stderr=subprocess.STDOUT, start_new_session=True)

    def _wait_ready(self):
        def up():
            if self.process.poll() is not None:
                raise LabError(f"Hyprland exited with {self.process.returncode}:\n{self._log_tail()}")
            return self.signature() and self.hyprctl("version")[0] == 0
        if not poll(up, START_TIMEOUT, 0.1):
            raise LabError(f"Hyprland did not start within {START_TIMEOUT:.0f}s:\n{self._log_tail()}")
        self.hyprctl("output", "create", "headless")
        if not poll(lambda: self.monitor(), 10, 0.1):
            raise LabError(f"no output came up:\n{self._log_tail()}")

    def _log_tail(self, lines=25):
        text = self.log.read_text(errors="replace") if self.log.exists() else ""
        return "\n".join(ANSI.sub("", line) for line in text.splitlines()[-lines:])

    def _stop_compositor(self):
        process, self.process = self.process, None
        if process is None:
            return
        # The compositor leads its own session and group, which holds only what it started. Signal the
        # group only while the leader is unreaped, so its id cannot have been given to anyone else.
        for stop in (signal.SIGTERM, signal.SIGKILL):
            if process.poll() is not None:
                break
            try:
                if os.getpgid(process.pid) == process.pid != os.getpgrp():
                    os.killpg(process.pid, stop)
                else:
                    process.send_signal(stop)
            except (ProcessLookupError, PermissionError):
                break
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                continue
        if self._log_file:
            self._log_file.close()

    # talking to the compositor

    def signature(self):
        if self._signature is None:
            found = sorted(path.parent.name for path in self.run_dir.glob("hypr/*/.socket.sock"))
            self._signature = found[0] if found else None
        return self._signature

    def _wayland_socket(self):
        sockets = poll(lambda: sorted(p for p in self.run_dir.glob("wayland-*") if p.suffix != ".lock"), 10)
        if not sockets:
            raise LabError("the compositor opened no Wayland socket")
        return str(sockets[0])

    def hyprctl(self, *args):
        """(exit code, output) of hyprctl against the lab compositor only."""
        env = {"PATH": "/usr/bin:/bin", "XDG_RUNTIME_DIR": str(self.run_dir),
               "HYPRLAND_INSTANCE_SIGNATURE": self.signature() or ""}
        done = subprocess.run([hyprctl_binary(), *args], env=env, capture_output=True, text=True, timeout=15)
        return done.returncode, done.stdout.strip()

    def json(self, *args):
        code, out = self.hyprctl("-j", *args)
        try:
            return json.loads(out) if code == 0 else None
        except ValueError:
            return None

    def dispatch(self, expression):
        return self.hyprctl("dispatch", expression)[1]

    @staticmethod
    def wait(predicate, timeout=1.0):
        return poll(predicate, timeout, 0.03)

    def submap(self):
        return self.hyprctl("submap")[1] or "default"

    def active(self):
        return self.json("activewindow") or {}

    def workspace(self):
        return self.json("activeworkspace") or {}

    def monitor(self):
        monitors = [m for m in self.json("monitors") or [] if not m.get("disabled")]
        return monitors[0] if monitors else {}

    def clients(self):
        return [c for c in self.json("clients") or [] if c.get("title", "").startswith("lab-")]

    def client(self, address):
        return next((c for c in self.clients() if c.get("address") == address), {})

    # windows and state

    def _reap_windows(self):
        for process in self.windows:
            if process.poll() is None:
                process.terminate()
        for process in self.windows:
            try:
                process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
        self.windows = []

    def _respawn(self):
        self._reap_windows()
        self.wait(lambda: not self.clients(), 3)
        env = {"PATH": "/usr/bin:/bin", "HOME": str(self.root / "home"), "XDG_RUNTIME_DIR": str(self.run_dir),
               "WAYLAND_DISPLAY": Path(self._wayland_socket()).name, "GDK_BACKEND": "wayland",
               "DBUS_SESSION_BUS_ADDRESS": "unix:path=/nonexistent", "GTK_A11Y": "none", "NO_AT_BRIDGE": "1",
               "LANG": "C.UTF-8"}
        for number in range(1, WINDOW_COUNT + 1):
            self.windows.append(subprocess.Popen(
                [sys.executable, "-c", WINDOW_SCRIPT, f"lab-{number}"], env=env, stdin=subprocess.DEVNULL,
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL))
            if not self.wait(lambda: len(self.clients()) >= number, 15):
                raise LabError(f"test window {number} did not appear:\n{self._log_tail()}")
        if not self.wait(lambda: all(c["mapped"] for c in self.clients()), 5):
            raise LabError("the test windows were never mapped")
        return self.clients()

    def focus_workspace(self, number):
        self.dispatch(f"hl.dsp.focus({{ workspace = {number} }})")

    def send_active_to(self, number):
        address = self.active().get("address")
        self.dispatch(f'hl.dsp.window.move({{ workspace = {number}, follow = false, window = "address:{address}" }})')
        self.focus_workspace(number)
        self.dispatch(f'hl.dsp.focus({{ window = "address:{address}" }})')

    def stage(self, submap="reset"):
        """The state every bind starts from: two plain tiled windows on workspace 1, the first focused."""
        self.dispatch(f"hl.dsp.submap({lua_string(submap)})")
        clients = self.clients()
        if len(clients) != WINDOW_COUNT or any(not c["mapped"] or c.get("grouped") for c in clients):
            clients = self._respawn()
        special = self.monitor().get("specialWorkspace", {})
        if special.get("id"):
            self.dispatch(f"hl.dsp.workspace.toggle_special({lua_string(special['name'].split(':', 1)[-1])})")
        for client in clients:
            window = f'window = "address:{client["address"]}"'
            if client.get("floating"):
                self.dispatch(f'hl.dsp.window.float({{ action = "disable", {window} }})')
            if client.get("pinned"):
                self.dispatch(f'hl.dsp.window.pin({{ action = "disable", {window} }})')
            if client.get("fullscreen") or client.get("fullscreenClient"):
                self.dispatch(f"hl.dsp.window.fullscreen_state({{ internal = 0, client = 0, {window} }})")
            if client["workspace"]["id"] != 1:
                self.dispatch(f"hl.dsp.window.move({{ workspace = 1, follow = false, {window} }})")
        self.focus_workspace(1)
        first = min(clients, key=lambda c: c["title"])
        self.dispatch(f'hl.dsp.focus({{ window = "address:{first["address"]}" }})')

    # pressing keys

    def mark(self):
        return self.log.stat().st_size, self.calls.stat().st_size

    def seen(self, marks):
        def text(path, mark):
            with open(path, "rb") as handle:
                handle.seek(mark)
                return handle.read().decode("utf-8", "replace")
        seen = Seen([], [], [], [], [])
        for raw in text(self.log, marks[0]).splitlines():
            line = ANSI.sub("", raw)
            if match := TRIGGER.search(line):
                seen.triggers.append((int(match.group(1)), int(match.group(2))))
            elif match := EXECUTED.search(line):
                seen.executed.append(match.group(1))
            elif (match := LEVELLED.match(line)) and not any(word in line for word in BENIGN):
                seen.errors.append(match.group(1))
        for line in text(self.calls, marks[1]).splitlines():
            (seen.results if line.startswith("result ") else seen.stubs).append(line)
        return seen

    def press(self, plan):
        marks = self.mark()
        self.keyboard.tap(plan.code, plan.mask)

        def done():
            seen = self.seen(marks)
            return seen.triggers and (plan.command is None or (seen.executed and seen.stubs))
        poll(done, TRIGGER_TIMEOUT)
        time.sleep(SETTLE)
        return self.seen(marks)


# ── Judging one bind ────────────────────────────────────────────────────────

def judge(plan, seen):
    """Why the bind misbehaved, or None."""
    chord = plan.entry.bind.label
    if seen.errors:
        return seen.errors[0]
    if not seen.triggers:
        return f"no bind fired for {chord}: it is shadowed, not registered, or Hyprland did not accept it"
    if len(seen.triggers) > 1:
        return f"{chord} fired {len(seen.triggers)} binds: the chord is bound more than once"
    mask, keysym = seen.triggers[0]
    if keysym != plan.keysym or mask != plan.mask:
        return f"fired with modifiers {mask} and keysym {keysym}, expected {plan.mask} and {plan.keysym}"
    if plan.command is not None:
        if seen.executed != [plan.command]:
            return f"ran {seen.executed or 'nothing'}, expected {plan.command!r}"
        first = plan.words[0]
        if not any(line == first or line.startswith(first + " ") for line in seen.stubs):
            return f"{first} never started"
        failed = [line for line in seen.results if not line.startswith("result 0 ") or not line.endswith(" ok")]
        if failed:
            return "hyprctl reported: " + failed[0].split(" ", 2)[-1]
    return None


def run(entries, sources, policy=False, keep=False, show=True):
    """Press each entry's chord in a lab compositor; one Result per entry, in order."""
    keymap = xkb.Keymap("us")
    plans, skipped = {}, {}
    for position, entry in enumerate(entries):
        plan, why = make_plan(entry, keymap)
        if plan:
            plans[position] = plan
        else:
            skipped[position] = Result(entry.bind.label, entry.bind.where, SKIP, why)
    results = []

    def record(result):
        results.append(result)
        if show:
            _report(result)

    if not plans:
        for position in range(len(entries)):
            record(skipped[position])
        return results
    commands = {word for plan in plans.values() for word in plan.words}
    with Lab(sources, policy, commands, keep) as lab:
        for position in range(len(entries)):
            record(skipped[position] if position in skipped else _press(lab, plans[position]))
    return results


def _press(lab, plan):
    bind = plan.entry.bind
    try:
        lab.stage(submap=bind.submap or "reset")
        state = plan.effect.stage(lab) if plan.effect and plan.effect.stage else None
        problem = judge(plan, lab.press(plan))
        if problem is None and plan.effect:
            problem = plan.effect.verify(lab, state)
    except (OSError, vkbd.WaylandError, subprocess.SubprocessError) as error:
        if lab.process is None or lab.process.poll() is not None:
            raise LabError(f"Hyprland died while pressing {bind.label}:\n{lab._log_tail()}") from error
        problem = f"the lab could not press it: {error}"
    return Result(bind.label, bind.where, FAIL if problem else PASS, problem or "")


def _report(result):
    mark = {PASS: "ok  ", FAIL: "FAIL", SKIP: "skip"}[result.status]
    detail = f"  {result.detail}" if result.detail else ""
    print(f"  {mark} {result.label:<28}{detail}", flush=True)
