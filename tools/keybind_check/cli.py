"""Command line of the smplOS keybinding checker; run it as tools/keybind-check."""
from __future__ import annotations

import argparse
import dataclasses
import json
import os
import re
import subprocess
import sys
from pathlib import Path

from . import lab, loader, model, native, rules, snapshot, xkb

POLICIES = {"off": (False,), "on": (True,), "both": (False, True)}
EXAMPLES = """\
examples:
  keybind-check check                      static + native checks on the stock binds
  keybind-check check --only SUPER+TAB     one chord (an exact chord wins over partial matches)
  keybind-check check --only "window management"     one group, or any text from a description
  keybind-check check --live               audit the binds this machine really loads
  keybind-check diff --live                how your bindings.conf differs from stock
  keybind-check lab                        press every chord in a headless Hyprland
  keybind-check snapshot --update          accept an intended change to the stock binds
  keybind-check doctor                     which checks can run here

The same checks run in the test suite; one bind: -k SUPER_TAB, one group: -k window_management
  PYTHONPATH=tests python3 -m unittest test_keybindings -v
"""
LIVE_HINT = """\
Update OS never rewrites a bindings.conf that was installed earlier (it is protected, because
you may have edited it); fixes to stock binds reach it as migrations. Run Update OS, then
`keybind-check check --live` again, and use `keybind-check diff --live` to see what differs."""


class UsageError(Exception):
    """A bad option or a missing file; reported without a traceback."""


# ── Choosing binds ──────────────────────────────────────────────────────────

def user_files():
    """The bind files hyprland.lua loads for this user: one bindings.conf, then messengers."""
    config = Path.home() / ".config"
    bindings = next((path for path in (config / "smplos/bindings.conf", config / "hypr/bindings.conf")
                     if path.is_file()), None)
    if bindings is None:
        raise UsageError("no bindings.conf in ~/.config/smplos or ~/.config/hypr")
    messenger = config / "hypr/messenger-bindings.conf"
    return bindings, messenger if messenger.is_file() else None


def _compact(text):
    return re.sub(r"[\s+]", "", text.lower())


def selector(patterns, binds):
    """Predicate for --only. An exact chord wins over text that merely contains it."""
    if not patterns:
        return lambda bind: True
    chosen = set()
    for pattern in patterns:
        needle = _compact(pattern)
        exact = [bind for bind in binds
                 if needle in (_compact(bind.label), _compact(bind.label.rsplit(": ", 1)[-1]))]
        loose = [bind for bind in binds if needle in _compact(" ".join(
            (bind.label, bind.group, bind.desc, bind.dispatcher or "", bind.raw_arg)))]
        if not (exact or loose):
            raise UsageError(f"no bind matches --only {pattern!r}")
        chosen.update(id(bind) for bind in exact or loose)
    return lambda bind: id(bind) in chosen


@dataclasses.dataclass
class Context:
    sources: list
    parsed: list
    custom: bool  # the binds do not come from this repository's stock file
    selected: object

    @property
    def binds(self):
        return model.all_binds(self.parsed)

    @property
    def chosen(self):
        return [bind for bind in self.binds if self.selected(bind)]


def build_context(args):
    bindings, messenger = args.bindings, args.messenger
    if args.live:
        live_bindings, live_messenger = user_files()
        bindings, messenger = bindings or live_bindings, messenger or live_messenger
    for path in (bindings, messenger):
        if path and not Path(path).is_file():
            raise UsageError(f"{path} does not exist")
    sources = model.default_sources(bindings=bindings, messenger=messenger)
    parsed = model.load_sources(sources)
    selected = selector(getattr(args, "only", None), model.all_binds(parsed))
    return Context(sources, parsed, bool(bindings or messenger), selected)


def add_binds_options(parser, only=True):
    group = parser.add_argument_group("which binds")
    group.add_argument("--bindings", metavar="FILE", help="use this bindings.conf, not the stock one")
    group.add_argument("--messenger", metavar="FILE", help="also load this messenger-bindings.conf")
    group.add_argument("--live", action="store_true",
                       help="use the bind files this user's session loads (~/.config)")
    if only:
        group.add_argument("--only", action="append", default=[], metavar="TEXT",
                           help="only binds matching TEXT: a chord such as SUPER+TAB, a group, or "
                                "words from the description or command (repeatable)")


# ── check ───────────────────────────────────────────────────────────────────

def _native_findings(args, ctx, entries, policies, notes):
    if not native.available():
        if args.strict:
            return [rules.finding("native", "Hyprland", f"Hyprland is not installed (set "
                                  f"{native.BINARY_ENV} to its path); --strict requires it")]
        notes.append("native check skipped: Hyprland is not installed")
        return []
    chosen = {id(bind) for bind in ctx.chosen}
    with native.Verifier(jobs=args.jobs) as verifier:
        notes.append(f"native: {verifier.version().split(' built')[0]}")
        findings = verifier.check([e for e in entries if id(e.bind) in chosen], policies)
        if not args.only and not ctx.custom:
            findings += verifier.check_file(model.POPUP_WATCHERS)
    return findings


def cmd_check(args):
    ctx = build_context(args)
    policies = POLICIES[args.policy]
    kind = args.commands or ("host" if ctx.custom else "repo")
    resolver = rules.HostResolver() if kind == "host" else rules.RepoResolver()
    findings, entries = [], None
    for policy in policies:
        recording = loader.record(ctx.sources, policy=policy)
        paired = loader.pair(ctx.parsed, recording)
        entries = entries or paired
        findings += rules.check_set(ctx.parsed, recording, paired, resolver,
                                    with_repo=not ctx.custom)
    notes = []
    if not args.no_native:
        findings += _native_findings(args, ctx, entries, policies, notes)
    findings = list(dict.fromkeys(findings))
    if args.only:
        places = {bind.where for bind in ctx.chosen}
        findings = [item for item in findings if item.where in places]
    failing = [item for item in findings if item.severity == rules.ERROR or args.warnings_as_errors]
    count = len(ctx.chosen)
    if args.json:
        print(json.dumps({"ok": not failing, "binds": count, "notes": notes,
                          "findings": [dataclasses.asdict(item) for item in findings]}, indent=2))
        return 1 if failing else 0
    for item in findings:
        print(item)
    for note in notes:
        print(f"note: {note}")
    scope = f"{count} bind(s), workspace policy {' and '.join(('off', 'on')[p] for p in policies)}"
    warnings = len(findings) - len(rules.errors(findings))
    if failing:
        print(f"FAIL: {len(rules.errors(findings))} error(s), {warnings} warning(s) in {scope}")
        if ctx.custom and args.live:
            print(LIVE_HINT)
    else:
        extra = f" ({warnings} warning(s))" if warnings else ""
        print(f"OK: {scope}{extra}")
    return 1 if failing else 0


# ── list ────────────────────────────────────────────────────────────────────

def cmd_list(args):
    ctx = build_context(args)
    recording = loader.record(ctx.sources, policy=args.policy == "on")
    bind_of = {id(e.call): e.bind for e in loader.pair(ctx.parsed, recording)}
    rows = []
    for call in recording.calls:
        bind = bind_of.get(id(call))
        if args.only and not (bind and ctx.selected(bind)):
            continue
        rows.append({
            "group": bind.group if bind else call.source,
            "chord": call.label, "action": snapshot.describe(call) + snapshot.options(call),
            "description": call.description,
            "where": f"{Path(bind.path).name}:{bind.line}" if bind else f"{call.source}.lua",
        })
    if args.json:
        print(json.dumps(rows, indent=2))
        return 0
    width = min(30, max((len(row["chord"]) for row in rows), default=0))
    group = None
    for row in rows:
        if row["group"] != group:
            group = row["group"]
            print(f"\n{group}")
        action = row["action"] if len(row["action"]) <= 46 else row["action"][:45] + "…"
        print(f"  {row['chord']:<{width}}  {action:<46}  {row['description']}  ({row['where']})")
    print(f"\n{len(rows)} bind(s)")
    return 0


# ── diff ────────────────────────────────────────────────────────────────────

def _calls_by_chord(path):
    source = [model.Source("stock", path)]
    calls = {}
    for call in loader.record(source, popup=False).calls:
        calls.setdefault(call.chord, []).append(call)
    return calls


def _show(calls):
    return " AND ".join(f"{snapshot.describe(c)}{snapshot.options(c)}" + (
        f"  ({c.description})" if c.description else "") for c in calls)


def cmd_diff(args):
    args.live = args.live or not args.bindings
    bindings = args.bindings or user_files()[0]
    if not Path(bindings).is_file():
        raise UsageError(f"{bindings} does not exist")
    stock, mine = _calls_by_chord(model.STOCK_BINDINGS), _calls_by_chord(bindings)
    rows, same = [], 0
    for chord, calls in stock.items():
        other = mine.get(chord)
        if other is None:
            rows.append(("missing", calls[0].label, f"stock: {_show(calls)}"))
        elif [snapshot.describe(c) + snapshot.options(c) for c in calls] != \
                [snapshot.describe(c) + snapshot.options(c) for c in other]:
            rows.append(("changed", calls[0].label, f"stock: {_show(calls)}\n"
                                                    f"{'':<9}yours: {_show(other)}"))
        else:
            same += 1
    rows += [("extra", calls[0].label, f"yours: {_show(calls)}")
             for chord, calls in mine.items() if chord not in stock]
    if args.json:
        print(json.dumps({"identical": same, "differences": [
            {"kind": kind, "chord": chord, "detail": detail} for kind, chord, detail in rows]}, indent=2))
        return 0
    for kind, chord, detail in rows:
        print(f"{kind:<8} {chord}\n{'':<9}{detail}")
    counts = {kind: sum(1 for row in rows if row[0] == kind) for kind in ("changed", "missing", "extra")}
    print(f"\n{same} identical to stock, {counts['changed']} changed, {counts['missing']} missing "
          f"from {model.display_path(bindings)}, {counts['extra']} only in it")
    return 0


# ── snapshot ────────────────────────────────────────────────────────────────

def cmd_snapshot(args):
    rendered = snapshot.render()
    if args.print:
        sys.stdout.write(rendered)
        return 0
    if args.update:
        path = snapshot.update(rendered)
        print(f"wrote {model.display_path(path)} ({len(rendered.splitlines())} lines)")
        return 0
    if not snapshot.SNAPSHOT.is_file():
        print(f"no snapshot at {model.display_path(snapshot.SNAPSHOT)} yet: "
              "run `tools/keybind-check snapshot --update`", file=sys.stderr)
        return 1
    delta = snapshot.diff(rendered)
    if not delta:
        print(f"OK: the stock binds match {model.display_path(snapshot.SNAPSHOT)}")
        return 0
    sys.stdout.write(delta)
    print("\nFAIL: the stock binds changed. If that is intended, run "
          "`tools/keybind-check snapshot --update` and commit the new snapshot.")
    return 1


# ── lab ─────────────────────────────────────────────────────────────────────

def cmd_lab(args):
    ctx = build_context(args)
    problems = lab.preflight()
    if problems:
        print("the lab cannot run here:", *problems, sep="\n  ", file=sys.stderr)
        return 2
    policy = args.policy == "on"
    recording = loader.record(ctx.sources, policy=policy)
    entries = [e for e in loader.pair(ctx.parsed, recording) if ctx.selected(e.bind)]
    results = lab.run(entries, ctx.sources, policy=policy, keep=args.keep, show=not args.json)
    failed = [r for r in results if r.status == lab.FAIL]
    if args.json:
        print(json.dumps([dataclasses.asdict(r) for r in results], indent=2))
    else:
        counts = {s: sum(1 for r in results if r.status == s) for s in (lab.PASS, lab.FAIL, lab.SKIP)}
        print(f"\n{'FAIL' if failed else 'OK'}: {counts[lab.PASS]} passed, {counts[lab.FAIL]} failed, "
              f"{counts[lab.SKIP]} skipped")
    return 1 if failed else 0


# ── doctor ──────────────────────────────────────────────────────────────────

def _lua_version(lua):
    done = subprocess.run([lua, "-v"], capture_output=True, text=True, timeout=10)
    return re.split(r"\s+(?:Copyright|--)", (done.stdout + done.stderr).strip())[0]


def _hyprland_version():
    with native.Verifier() as verifier:
        return verifier.version().split(" built")[0]


def _located(path, version):
    """`path (version)`, or just the path when the tool will not report a version."""
    try:
        text = version()
    except (OSError, subprocess.SubprocessError):
        text = ""
    return f"{path} ({text})" if text else path


def cmd_doctor(args):
    lua = loader.lua_binary()
    hyprland = native.binary()
    lab_problems = lab.preflight()
    rows = [
        ("Lua (loader parity, static rules)",
         _located(lua, lambda: _lua_version(lua)) if lua else "MISSING: install lua or lua5.4"),
        ("libxkbcommon (chord identity, lab typing)",
         "found" if xkb.available() else "missing: falls back to name matching; the lab needs it"),
        ("Hyprland (native per-bind verification)",
         _located(hyprland, _hyprland_version) if hyprland else "missing: native checks are skipped"),
        ("lab prerequisites", "; ".join(lab_problems) or "ready"),
    ]
    for name, state in rows:
        print(f"{name:<44} {state}")
    return 0 if lua else 2


# ── entry point ─────────────────────────────────────────────────────────────

def build_parser():
    parser = argparse.ArgumentParser(
        prog="keybind-check", description="Check every smplOS keyboard shortcut.",
        epilog=EXAMPLES, formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest="command", required=True, metavar="COMMAND")

    check = commands.add_parser("check", help="static rules + Hyprland's own parser (default for CI)")
    add_binds_options(check)
    check.add_argument("--policy", choices=POLICIES, default="both",
                       help="workspace policy mode(s) to load the binds in (default: both)")
    check.add_argument("--no-native", action="store_true", help="skip the Hyprland --verify-config pass")
    check.add_argument("--strict", action="store_true", help="fail, not skip, when Hyprland is missing")
    check.add_argument("-W", "--warnings-as-errors", action="store_true")
    check.add_argument("--commands", choices=("repo", "host"),
                       help="resolve exec commands against this repository (default for stock) or "
                            "this machine's PATH (default for --bindings/--live)")
    check.add_argument("--jobs", type=int, help="parallel Hyprland processes (default: up to 8)")
    check.add_argument("--json", action="store_true")
    check.set_defaults(run=cmd_check)

    listing = commands.add_parser("list", help="print every bind and what it does")
    add_binds_options(listing)
    listing.add_argument("--policy", choices=("off", "on"), default="off")
    listing.add_argument("--json", action="store_true")
    listing.set_defaults(run=cmd_list)

    diff = commands.add_parser("diff", help="compare a bindings.conf with the stock one")
    add_binds_options(diff, only=False)
    diff.add_argument("--json", action="store_true")
    diff.set_defaults(run=cmd_diff)

    snap = commands.add_parser("snapshot", help="compare, or update, the committed bind snapshot")
    snap.add_argument("--update", action="store_true", help="rewrite tests/fixtures/keybindings.snapshot.txt")
    snap.add_argument("--print", action="store_true", help="print the current snapshot")
    snap.set_defaults(run=cmd_snapshot)

    lab_parser = commands.add_parser("lab", help="press every chord in a headless Hyprland (local, needs a GPU)")
    add_binds_options(lab_parser)
    lab_parser.add_argument("--policy", choices=("off", "on"), default="off")
    lab_parser.add_argument("--keep", action="store_true", help="keep the lab's logs and files afterwards")
    lab_parser.add_argument("--json", action="store_true")
    lab_parser.set_defaults(run=cmd_lab)

    doctor = commands.add_parser("doctor", help="show which checks can run on this machine")
    doctor.set_defaults(run=cmd_doctor)
    return parser


def main(argv=None):
    args = build_parser().parse_args(argv)
    try:
        return args.run(args)
    except UsageError as error:
        print(f"keybind-check: {error}", file=sys.stderr)
        return 2
    except (loader.LoaderError, lab.LabError, FileNotFoundError, OSError) as error:
        print(f"keybind-check: {error}", file=sys.stderr)
        return 2
    except BrokenPipeError:
        os.dup2(os.open(os.devnull, os.O_WRONLY), sys.stdout.fileno())
        return 0
