"""Text snapshot of every bind the stock config registers.

The snapshot lists what bindings_loader.lua hands to Hyprland for each stock bind, so any
change to a shortcut, including one a merge or a revert makes silently, shows up as a diff a
reviewer can read. Regenerate it with `tools/keybind-check snapshot --update`.
"""
from __future__ import annotations

import difflib
import json

from . import loader, model

SNAPSHOT = model.ROOT / "tests/fixtures/keybindings.snapshot.txt"
HEADER = """\
# smplOS keybinding snapshot: every bind the stock config registers, as bindings_loader.lua
# translates it for Hyprland. Regenerate with: tools/keybind-check snapshot --update
# Columns: source | chord | description | text in the bind file -> hl.bind() action {options}
# A "policy on" line shows what a workspace-policy session gets where it differs.
"""


def _json(value):
    return json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=False)


def describe(call):
    """The action of an hl.bind() call, such as `layout("focus d")`."""
    if call.is_closure:
        return "<function>"
    return f"{call.name}({', '.join(_json(arg) for arg in call.args)})"


def options(call):
    extra = {key: value for key, value in call.opts.items() if key != "description"}
    return f" {_json(extra)}" if extra else ""


def render(sources=None):
    """The snapshot text for the stock bind set."""
    sources = sources or model.default_sources()
    parsed = model.load_sources(sources)
    plain = loader.record(sources, policy=False)
    policy = loader.record(sources, policy=True)
    entries = loader.pair(parsed, plain)
    if [(c.source, c.submap, c.keys) for c in plain.calls] != \
            [(c.source, c.submap, c.keys) for c in policy.calls]:
        raise loader.ParityError("the workspace policy changes which keys are bound")
    written = {id(entry.call): entry.bind for entry in entries}
    lines = []
    for call, variant in zip(plain.calls, policy.calls):
        bind = written.get(id(call))
        text = f"{bind.dispatcher} {bind.raw_arg}".strip() if bind else "<lua>"
        lines.append(f"{call.source} | {call.label} | {call.description} | "
                     f"{text} -> {describe(call)}{options(call)}")
        if (describe(variant), options(variant)) != (describe(call), options(call)):
            lines.append(f"    policy on -> {describe(variant)}{options(variant)}")
    return HEADER + "\n".join(lines) + "\n"


def committed():
    return SNAPSHOT.read_text(encoding="utf-8") if SNAPSHOT.is_file() else ""


def diff(rendered=None):
    """Unified diff from the committed snapshot to the current stock binds ("" when equal)."""
    rendered = render() if rendered is None else rendered
    return "".join(difflib.unified_diff(
        committed().splitlines(True), rendered.splitlines(True),
        "committed snapshot", "current stock binds"))


def update(rendered=None):
    rendered = render() if rendered is None else rendered
    SNAPSHOT.parent.mkdir(parents=True, exist_ok=True)
    SNAPSHOT.write_text(rendered, encoding="utf-8")
    return SNAPSHOT
