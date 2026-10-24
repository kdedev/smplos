#!/bin/bash
# Migration: add the scrolling-layout "pan view" keybinds (Super+Shift+] and [).
#
# Context: the smpl-scroll compositor plugin lets a column grow wider than the
#          monitor, and these binds scroll the view across it. They ship in the
#          stock bindings.conf, which is user/Settings-owned, so Update OS never
#          overwrites it. EXISTING installs would miss the binds; this adds them.
#
# Safety:  Insert-only and idempotent. Nothing is added unless the stock
#          scrolling navigation binds are present, no pixel-pan bind exists yet
#          and the key chord is free in the global scope; a customized chord is
#          never replaced. A changed file gets a verified backup first, and a
#          symlinked file is left alone.
set -euo pipefail

python3 - <<'PY'
import os
from pathlib import Path
import re
import stat
import tempfile

HEADER = (
    "# Pan the view in steps (hold to repeat) -- how to see the rest of a column",
    "# that is wider than the monitor (see the smpl-scroll plugin).",
)
PAN = (
    ("bracketright", "bindde = SUPER SHIFT, bracketright, Pan view right, layoutmsg, move -400"),
    ("bracketleft", "bindde = SUPER SHIFT, bracketleft, Pan view left, layoutmsg, move +400"),
)
NAVIGATION = ("move +col", "move -col", "center")


def global_binds(lines):
    scope = "reset"
    for index, line in enumerate(lines):
        submap = re.fullmatch(r"\s*submap\s*=\s*(\S+)\s*", line)
        if submap:
            scope = submap[1]
        match = re.match(r"\s*(bind[a-z]*)\s*=\s*(.*)", line)
        if scope != "reset" or not match:
            continue
        fields = [part.strip() for part in match[2].split(",")]
        offset = 3 if "d" in match[1][4:] else 2
        if len(fields) <= offset:
            continue
        yield (index, {m for m in re.split(r"[\s_+]+", fields[0].upper()) if m},
               fields[1].upper(), fields[offset], ", ".join(fields[offset + 1:]))


def holds_chord(mods, key, wanted):
    # An unexpanded $variable could mean SUPER, so it counts as taken.
    return key == wanted.upper() and "SHIFT" in mods and (
        mods == {"SUPER", "SHIFT"} or any(m.startswith("$") for m in mods))


def insert_pan_binds(path):
    original = path.read_bytes()
    text = original.decode("utf-8", errors="surrogateescape")
    lines = text.splitlines(keepends=True)
    binds = list(global_binds(lines))
    navigation = [b for b in binds if b[3] == "layoutmsg" and b[4] in NAVIGATION]
    if not any(b[4] != "center" for b in navigation):
        print(f"  {path}: no scrolling navigation binds, skipping")
        return
    if any(b[3] == "layoutmsg" and re.fullmatch(r"move [+-]?\d+", b[4]) for b in binds):
        print(f"  {path}: already has pan binds, skipping")
        return
    wanted = []
    for key, line in PAN:
        if any(holds_chord(mods, bound, key) for _, mods, bound, _, _ in binds):
            print(f"  WARNING: {path}: preserving customized SUPER+SHIFT+{key}; its pan bind is not added")
        else:
            wanted.append(line)
    if not wanted:
        return
    if path.is_symlink():
        print(f"  WARNING: {path} is a symlink; not modifying it. Add the pan binds from the stock bindings.conf by hand.")
        return
    ending = "\r\n" if "\r\n" in text else "\n"
    anchor = max(b[0] for b in navigation)
    if not lines[anchor].endswith("\n"):
        lines[anchor] += ending
    lines[anchor + 1:anchor + 1] = [line + ending for line in (*HEADER, *wanted)]
    updated = "".join(lines).encode("utf-8", errors="surrogateescape")

    before = path.stat()
    mode = stat.S_IMODE(before.st_mode)
    descriptor, backup = tempfile.mkstemp(prefix=path.name + ".pre-pan-binds.", dir=path.parent)
    with os.fdopen(descriptor, "wb") as stream:
        stream.write(original)
        stream.flush()
        os.fchmod(stream.fileno(), mode)
        os.fsync(stream.fileno())
    if Path(backup).read_bytes() != original:
        raise SystemExit(f"ERROR: binding backup verification failed: {backup}")
    descriptor, temporary = tempfile.mkstemp(prefix=path.name + ".tmp.", dir=path.parent)
    try:
        with os.fdopen(descriptor, "wb") as stream:
            stream.write(updated)
            stream.flush()
            os.fchmod(stream.fileno(), mode)
            os.fsync(stream.fileno())
        if path.stat() != before or path.read_bytes() != original:
            raise SystemExit(f"ERROR: bindings changed during migration; not overwriting {path}")
        os.replace(temporary, path)
    finally:
        Path(temporary).unlink(missing_ok=True)
    directory = os.open(path.parent, os.O_RDONLY | os.O_DIRECTORY)
    try:
        os.fsync(directory)
    finally:
        os.close(directory)
    print(f"  Added {len(wanted)} pan-view bind(s) to {path} (verified backup: {backup})")


found = False
for relative in (".config/smplos/bindings.conf", ".config/hypr/bindings.conf"):
    path = Path.home() / relative
    if path.exists():
        found = True
        insert_pan_binds(path)
if not found:
    print("  No bindings.conf found, nothing to do (new installs get the binds from the ISO)")
PY
