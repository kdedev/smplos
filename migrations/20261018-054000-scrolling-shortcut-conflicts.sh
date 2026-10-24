#!/bin/bash
# Repair stock scrolling shortcuts without replacing user-owned bindings.
set -euo pipefail

python3 - <<'PY'
import os
from pathlib import Path
import re
import stat
import tempfile


def bindings(lines):
    scope = "reset"
    for index, line in enumerate(lines):
        submap = re.fullmatch(r"\s*submap\s*=\s*(\S+)\s*", line)
        if submap:
            scope = submap[1]
        match = re.match(r"\s*(bind[a-z]*)\s*=\s*(.*)", line)
        if not match:
            continue
        fields = tuple(part.strip() for part in match[2].split(","))
        if len(fields) < 3:
            continue
        prefix = match[1]
        chord = (scope, frozenset(fields[0].upper().split()), fields[1].upper(),
                 "r" in prefix[4:], "m" in prefix[4:])
        yield index, chord, prefix, fields


replacements = {
    ("SUPER SHIFT", "LEFT", "Move column left", "layoutmsg", "movewindow l"):
        "SUPER CTRL SHIFT, LEFT, Move column left, layoutmsg, swapcol l",
    ("SUPER SHIFT", "RIGHT", "Move column right", "layoutmsg", "movewindow r"):
        "SUPER CTRL SHIFT, RIGHT, Move column right, layoutmsg, swapcol r",
    ("SUPER", "R", "Resize mode (arrow keys to resize - Esc to exit)", "submap", "resize"):
        "SUPER CTRL, R, Resize mode (arrow keys to resize - Esc to exit), submap, resize",
    ("SUPER", "J", "Cycle layout orientation (left/top/right/bottom)", "layoutmsg", "orientationnext"):
        None,
    ("SUPER", "S", "Toggle scratchpad", "togglespecialworkspace", "scratchpad"):
        "SUPER CTRL, S, Toggle scratchpad, togglespecialworkspace, scratchpad",
}
for modifiers in ("SUPER CTRL", "SUPER SHIFT"):
    for command in ('toggle-messenger "signal" signal-desktop', "toggle-messenger signal signal-desktop"):
        replacements[(modifiers, "S", "Signal", "exec", command)] = (
            'SUPER, S, Signal, exec, toggle-messenger "signal" signal-desktop'
        )
stock = {(frozenset(fields[0].split()), fields[1], *fields[2:]): replacement
         for fields, replacement in replacements.items()}

for relative in (".config/smplos/bindings.conf", ".config/hypr/bindings.conf"):
    path = Path.home() / relative
    if not path.exists():
        continue
    original = path.read_bytes()
    lines = original.decode("utf-8").splitlines(keepends=True)
    entries = list(bindings(lines))
    planned = {
        index for index, chord, prefix, fields in entries
        if chord[0] == "reset" and prefix == "bindd"
        and (chord[1], chord[2], *fields[2:]) in stock
    }
    occupied = {}
    for index, chord, prefix, fields in entries:
        if index not in planned:
            occupied.setdefault(chord, []).append((prefix, fields[2:]))
    scratch_index = next((
        index for index, _, _, fields in entries
        if index in planned and fields[2:] == ("Toggle scratchpad", "togglespecialworkspace", "scratchpad")
    ), None)
    add_signal = scratch_index is not None and not any(
        chord[0] == "reset" and prefix == "bindd" and fields[2] == "Signal" and fields[3] == "exec"
        for _, chord, prefix, fields in entries if len(fields) >= 5
    )
    if add_signal:
        index = len(lines)
        lines.append("")
        _, chord, prefix, fields = next(bindings([
            'bindd = SUPER CTRL, S, Signal, exec, toggle-messenger "signal" signal-desktop'
        ]))
        entries.append((index, chord, prefix, fields))
    for index, chord, prefix, fields in entries:
        identity = (chord[1], chord[2], *fields[2:])
        if chord[0] != "reset" or prefix != "bindd" or identity not in stock:
            continue
        replacement = stock[identity]
        if replacement is None:
            lines[index] = ""
            continue
        new_line = "bindd = " + replacement
        _, target, new_prefix, new_fields = next(bindings([new_line]))
        if target in occupied:
            if occupied[target] != [(new_prefix, new_fields[2:])]:
                print(f"  WARNING: {path}: preserving customized {new_fields[0]}+{new_fields[1]}; "
                      f"removing conflicting stock {fields[2]} without rebinding it")
            lines[index] = ""
        else:
            ending = "\r\n" if lines[index].endswith("\r\n") else "\n"
            lines[index] = new_line + ending
            occupied[target] = [(new_prefix, new_fields[2:])]
    if add_signal:
        lines[scratch_index] += lines.pop()
    updated = "".join(lines).encode("utf-8")
    if updated == original:
        continue
    if path.is_symlink():
        raise SystemExit(f"ERROR: refusing to replace symlinked bindings: {path}")
    before = path.stat()
    mode = stat.S_IMODE(before.st_mode)
    descriptor, backup = tempfile.mkstemp(prefix=path.name + ".pre-scrolling-shortcuts.", dir=path.parent)
    with os.fdopen(descriptor, "wb") as stream:
        stream.write(original)
        stream.flush()
        os.fchmod(stream.fileno(), mode)
        os.fsync(stream.fileno())
    if Path(backup).read_bytes() != original:
        raise SystemExit(f"ERROR: binding backup verification failed: {backup}")
    directory = os.open(path.parent, os.O_RDONLY | os.O_DIRECTORY)
    try:
        os.fsync(directory)
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
            os.fsync(directory)
        finally:
            Path(temporary).unlink(missing_ok=True)
    finally:
        os.close(directory)
    print(f"  Repaired scrolling shortcuts in {path} (verified backup: {backup})")
PY

# Regenerate protected messenger output so the old Signal binding cannot coexist
# with the new explicit one. No compositor or session restart is performed.
if [[ -f "$HOME/.config/smplos/bindings.conf" || -f "$HOME/.config/hypr/bindings.conf" ]]; then
  bash "$(dirname "${BASH_SOURCE[0]}")/../src/shared/bin/generate-messenger-bindings"
fi
