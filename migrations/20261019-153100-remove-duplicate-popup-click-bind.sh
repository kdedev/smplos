#!/bin/bash
# Migration: stop binding the popup click-outside watcher twice.
#
# Context: every left click runs popup-click-check, which closes a popup the click
#          missed. hypr/popup_watchers.lua registers that bind, and older stock
#          bindings.conf files declared it a second time, so each click ran the
#          script twice. The stock file no longer has the bindn line, but
#          bindings.conf is user/Settings-owned and Update OS never rewrites it,
#          so existing installs keep the duplicate. This removes it.
#
# Safety:  Only the stock line (bindn = , mouse:272, exec, popup-click-check) in
#          the global scope is removed, and only while the Lua config really loads
#          hypr/popup_watchers.lua, which registers the same bind; without that
#          the line is the only click-outside handler and stays. The stock comment
#          block above it goes too; a customized comment is kept and a short note
#          takes the bind's place. A changed file gets a verified backup first,
#          and a symlinked file is left alone.
set -euo pipefail

python3 - <<'PY'
import os
from pathlib import Path
import re
import stat
import tempfile

HYPR = Path.home() / ".config/hypr"
BANNER = "# ============================================================================"
TITLE = "# Popup click-outside-to-dismiss (start-menu, calendar)"
# Both headers that ever shipped; they differ only in the last three comment lines.
OLD_HEADERS = tuple(
    (BANNER, TITLE, BANNER,
     "# Non-consuming left-click watcher: every click runs popup-click-check, which", *tail)
    for tail in (
        ("# closes any open popup if the click landed outside it. Lives here (not in the",
         "# .lua loader) because Hyprland 0.54 loads the .conf config; the .lua mirror is",
         "# only used after the 0.55 migration. Internal \u2014 not shown in keybind-help."),
        ("# closes any open popup if the click landed outside it. Declared in bindings.conf",
         "# (the single source of truth); the .lua loader parses it into an hl.bind on",
         "# Hyprland 0.55+. Internal \u2014 not shown in keybind-help."),
    )
)
NOTE = (
    "# Popup click-outside-to-dismiss (popup-click-check on every left click) is not",
    "# a keybinding: hypr/popup_watchers.lua registers it. Binding mouse:272 here as",
    "# well would run it twice per click.",
)


def lua_code(path):
    try:
        text = path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return ""
    return re.sub(r"--[^\n]*", "", re.sub(r"--\[(=*)\[.*?\]\1\]", "", text, flags=re.S))


def watcher_is_loaded():
    entry = lua_code(HYPR / "hyprland.lua")
    watcher = lua_code(HYPR / "popup_watchers.lua")
    return bool(
        re.search(r"(?m)^[ \t]*(?:pcall\s*\(\s*)?require\s*[\s(,]\s*[\"']popup_watchers[\"']", entry)
        and re.search(r"hl\.bind\s*\(\s*[\"']mouse:272[\"'].{0,200}?popup-click-check", watcher, re.S))


def duplicate_binds(lines):
    scope = "reset"
    for index, line in enumerate(lines):
        submap = re.fullmatch(r"\s*submap\s*=\s*(\S+)\s*", line)
        if submap:
            scope = submap[1]
        match = re.match(r"\s*(bind[a-z]*)\s*=\s*(.*)", line)
        if scope != "reset" or not match or match[1] != "bindn":
            continue
        fields = [part.strip() for part in match[2].split(",")]
        if [field.lower() for field in fields[:3]] == ["", "mouse:272", "exec"] \
                and fields[3:] == ["popup-click-check"]:
            yield index


def ending_of(line):
    return line[len(line.rstrip("\r\n")):]


def stock_block_start(lines, index):
    """Index of the blank line above a verbatim stock header, or None."""
    for header in OLD_HEADERS:
        start = index - len(header)
        if start >= 1 and not lines[start - 1].strip() \
                and tuple(line.rstrip("\r\n") for line in lines[start:index]) == header:
            return start - 1
    return None


def replace_safely(path, original, updated):
    before = path.stat()
    mode = stat.S_IMODE(before.st_mode)
    descriptor, backup = tempfile.mkstemp(prefix=path.name + ".pre-popup-bind.", dir=path.parent)
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
    return backup


def remove_duplicate(path):
    original = path.read_bytes()
    text = original.decode("utf-8", errors="surrogateescape")
    lines = text.splitlines(keepends=True)
    found = list(duplicate_binds(lines))
    if not found:
        print(f"  {path}: no duplicate popup click bind, nothing to do")
        return
    if not watcher_is_loaded():
        print(f"  {path}: keeping the popup click bind; hypr/hyprland.lua does not load "
              "hypr/popup_watchers.lua, so it is the only click-outside handler")
        return
    if path.is_symlink():
        print(f"  WARNING: {path} is a symlink; not modifying it. "
              "Delete its 'bindn = , mouse:272, exec, popup-click-check' line by hand.")
        return
    default = "\r\n" if "\r\n" in text else "\n"
    for index in reversed(found):
        start = stock_block_start(lines, index)
        first, block = (index, NOTE) if start is None else (start, ("#", *NOTE))
        last = ending_of(lines[index])
        lines[first:index + 1] = [*(line + (last or default) for line in block[:-1]), block[-1] + last]
    backup = replace_safely(path, original, "".join(lines).encode("utf-8", errors="surrogateescape"))
    print(f"  Removed the duplicate popup click bind from {path} (verified backup: {backup})")


seen = False
for relative in (".config/smplos/bindings.conf", ".config/hypr/bindings.conf"):
    path = Path.home() / relative
    if path.exists():
        seen = True
        remove_duplicate(path)
if not seen:
    print("  No bindings.conf found, nothing to do (new installs do not have the duplicate)")
PY
