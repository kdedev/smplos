"""Run the real bindings_loader.lua over bind files and record what it registers."""
from __future__ import annotations

import dataclasses
import json
import shutil
import subprocess

from . import model

DRIVER = model.ROOT / "tools/keybind_check/driver.lua"
CLOSURE = "<function>"
UNMAPPED_PREFIX = "[bindings_loader] unmapped dispatcher"


class LoaderError(RuntimeError):
    """The Lua driver could not run or produced unusable output."""


class ParityError(LoaderError):
    """The Python bind model and the real loader disagree about what a file registers."""


def lua_binary():
    return shutil.which("lua") or shutil.which("lua5.4") or shutil.which("luajit")


@dataclasses.dataclass(frozen=True)
class Call:
    source: str
    submap: str
    keys: str
    name: str
    args: tuple
    opts: dict
    enabled: bool

    @property
    def is_closure(self):
        return self.name == CLOSURE

    @property
    def chord(self):
        return model.chord_from_keys(self.submap, self.keys, bool(self.opts.get("release")))

    @property
    def label(self):
        tokens = [model.trim(token) for token in self.keys.split("+")]
        return model.chord_label(self.submap, tokens[:-1], tokens[-1])

    @property
    def description(self):
        return self.opts.get("description", "")


@dataclasses.dataclass(frozen=True)
class Recording:
    calls: tuple
    warnings: tuple

    def for_source(self, name):
        return [call for call in self.calls if call.source == name]


def _call(raw):
    action = raw["action"]
    opts = raw.get("opts") or {}
    return Call(raw["source"], raw["submap"], raw["keys"], action["name"],
                tuple(action.get("args") or ()), opts if isinstance(opts, dict) else {},
                bool(raw.get("enabled", True)))


def record(sources, policy=False, popup=True):
    """Load `sources` through the real loader (workspace policy on or off)."""
    lua = lua_binary()
    if not lua:
        raise LoaderError("Lua is not installed")
    command = [lua, str(DRIVER), str(model.LOADER), "1" if policy else "0",
               str(model.POPUP_WATCHERS) if popup else "-"]
    command += [f"{source.name}={source.path}" for source in sources]
    result = subprocess.run(command, capture_output=True, timeout=30)
    stderr = result.stderr.decode("utf-8", "replace")
    if result.returncode != 0:
        raise LoaderError(f"bindings_loader.lua failed ({result.returncode}):\n{stderr}")
    try:
        document = json.loads(result.stdout.decode("utf-8", "replace"))
    except ValueError as error:
        raise LoaderError(f"unreadable loader output: {error}\n{stderr}") from error
    warnings = tuple(line for line in stderr.splitlines() if line.startswith(UNMAPPED_PREFIX))
    return Recording(tuple(_call(raw) for raw in document["calls"]), warnings)


@dataclasses.dataclass(frozen=True)
class Entry:
    """A bind line together with the hl.bind() call the loader made for it."""
    bind: model.Bind
    call: Call


def pair(parsed, recording):
    """Match every registered bind line with its recorded call, in file order."""
    entries = []
    for item in parsed:
        binds = [bind for bind in item.binds if bind.registered]
        calls = recording.for_source(item.source.name)
        if len(binds) != len(calls):
            raise ParityError(
                f"{item.source.name}: model found {len(binds)} bind(s), the loader registered "
                f"{len(calls)}")
        for bind, call in zip(binds, calls):
            if (bind.submap, bind.combo) != (call.submap, call.keys):
                raise ParityError(
                    f"{bind.where}: model reads ({bind.submap!r}, {bind.combo!r}) but the loader "
                    f"registered ({call.submap!r}, {call.keys!r})")
            entries.append(Entry(bind, call))
    return entries
