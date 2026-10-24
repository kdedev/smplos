"""Bind files as bindings_loader.lua reads them, and the order Hyprland loads them in.

The splitting rules here deliberately mirror the loader (every comma splits, every
field is trimmed, the argument is the re-joined remainder). A test compares this
model with what the real loader registers, so the two cannot drift silently.
"""
from __future__ import annotations

import dataclasses
import re
from pathlib import Path

from . import xkb

ROOT = Path(__file__).resolve().parents[2]
HYPR_DIR = ROOT / "src/compositors/hyprland/hypr"
STOCK_BINDINGS = ROOT / "src/shared/configs/smplos/bindings.conf"
LOADER = HYPR_DIR / "bindings_loader.lua"
POPUP_WATCHERS = HYPR_DIR / "popup_watchers.lua"
HYPRLAND_LUA = HYPR_DIR / "hyprland.lua"
LOOKNFEEL_LUA = HYPR_DIR / "looknfeel.lua"

# Flag letters bindings_loader.lua turns into options; any other letter is silently dropped.
LOADER_FLAGS = "delmrin"

# Modifier names Hyprland's hl.bind accepts. The match is case-sensitive upstream.
MOD_NAMES = {
    "SHIFT": "SHIFT", "CAPS": "CAPS", "CTRL": "CTRL", "CONTROL": "CTRL",
    "ALT": "ALT", "MOD1": "ALT", "MOD2": "MOD2", "MOD3": "MOD3",
    "SUPER": "SUPER", "WIN": "SUPER", "LOGO": "SUPER", "MOD4": "SUPER",
    "META": "SUPER", "MOD5": "MOD5",
}
MOD_ORDER = ("SUPER", "CTRL", "ALT", "SHIFT", "CAPS", "MOD2", "MOD3", "MOD5")
SPECIAL_KEYS = ("mouse_down", "mouse_up", "mouse_left", "mouse_right")

# The loader maps the digit-row keycodes to keysyms: code:10 -> "1" ... code:19 -> "0".
CODE_TO_KEYSYM = {f"code:{10 + i}": str((i + 1) % 10) for i in range(10)}

_BIND_LINE = re.compile(r"^(bind[a-z]*)\s*=\s*(.+)$", re.ASCII)
_SUBMAP_LINE = re.compile(r"^submap\s*=\s*(.+)$", re.ASCII)
_RULE = re.compile(r"^#\s*=+\s*$", re.ASCII)

# Lua's %s: the loader trims and splits on exactly these, not on Unicode whitespace.
LUA_SPACE = " \t\n\v\f\r"


def trim(text):
    return text.strip(LUA_SPACE)


def words(text):
    """Whitespace-separated tokens, as Lua's gmatch("%S+") sees them."""
    return re.findall(r"[^ \t\n\v\f\r]+", text)


def slug(text, limit=None):
    """Identifier-safe form of text; with a limit, whole words only."""
    words = [word for word in re.split(r"[^A-Za-z0-9]+", text) if word]
    out = "_".join(words)
    if limit and len(out) > limit:
        out = words[0][:limit]
        for word in words[1:]:
            if len(out) + 1 + len(word) > limit:
                break
            out += "_" + word
    return out


@dataclasses.dataclass(frozen=True)
class Source:
    name: str
    path: Path
    group: str = ""  # group for binds that precede any section header


def default_sources(bindings=None, messenger=None):
    """Bind files in the order hyprland.lua loads them."""
    sources = [Source("stock", Path(bindings) if bindings else STOCK_BINDINGS)]
    if messenger:
        sources.append(Source("messenger", Path(messenger), "messenger"))
    sources.append(Source("xr", HYPR_DIR / "xr-workspace.conf", "xr"))
    sources.append(Source("emergency", HYPR_DIR / "emergency.conf", "emergency"))
    return sources


def display_path(path):
    path = Path(path)
    try:
        return str(path.resolve().relative_to(ROOT))
    except ValueError:
        return str(path)


@dataclasses.dataclass(frozen=True)
class Chord:
    submap: str
    mods: frozenset
    key: object  # keysym int when libxkbcommon knows the name, else a lowercase string
    release: bool


@dataclasses.dataclass(frozen=True)
class Bind:
    source: str
    path: str
    line: int
    raw: str
    keyword: str
    rhs: str
    fields: tuple
    submap: str
    group: str

    @property
    def flags(self):
        return self.keyword[4:]

    @property
    def has_desc(self):
        return "d" in self.flags

    @property
    def mods(self):
        return self.fields[0]

    @property
    def key(self):
        return self.fields[1] if len(self.fields) > 1 else ""

    @property
    def desc(self):
        return self.fields[2] if self.has_desc and len(self.fields) > 2 else ""

    @property
    def dispatcher(self):
        index = 3 if self.has_desc else 2
        return self.fields[index] if len(self.fields) > index else None

    @property
    def arg(self):
        """The argument as the loader rebuilds it: remaining fields re-joined with commas."""
        index = 4 if self.has_desc else 3
        return trim(",".join(self.fields[index:]))

    @property
    def raw_arg(self):
        """The argument exactly as written in the file."""
        count = 4 if self.has_desc else 3
        parts = self.rhs.split(",", count)
        return trim(parts[count]) if len(parts) > count else ""

    @property
    def registered(self):
        """The loader only registers a bind that has mods, key and dispatcher fields."""
        return self.dispatcher is not None

    @property
    def key_name(self):
        return CODE_TO_KEYSYM.get(self.key, self.key)

    @property
    def combo(self):
        """The key string handed to hl.bind."""
        return " + ".join(words(self.mods) + [self.key_name])

    @property
    def chord(self):
        return chord_from_keys(self.submap, self.combo, "r" in self.flags)

    @property
    def label(self):
        return chord_label(self.submap, words(self.mods), self.key)

    @property
    def where(self):
        return f"{self.path}:{self.line}"


def canonical_key(name):
    lowered = name.lower()
    if lowered.startswith(("mouse:", "switch:")) or lowered in SPECIAL_KEYS:
        return lowered
    if xkb.available():
        sym = xkb.canonical(name)
        if sym:
            return sym
    return lowered


def chord_label(submap, mods, key):
    """Readable name of a chord, such as "SUPER+SHIFT+K" ("resize: LEFT" inside a submap)."""
    names = {MOD_NAMES.get(token, token) for token in mods}
    ordered = [m for m in MOD_ORDER if m in names] + sorted(names - set(MOD_ORDER))
    text = "+".join(ordered + [key])
    return f"{submap}: {text}" if submap else text


def chord_from_keys(submap, keys, release):
    """Chord for an hl.bind key string such as "SUPER + SHIFT + K"."""
    tokens = [trim(token) for token in keys.split("+")]
    mods = frozenset(MOD_NAMES.get(token, token) for token in tokens[:-1])
    return Chord(submap, mods, canonical_key(tokens[-1]), release)


@dataclasses.dataclass(frozen=True)
class SubmapBlock:
    name: str
    line: int
    closed: bool


@dataclasses.dataclass
class Parsed:
    source: Source
    binds: list
    submaps: list


def _section_titles(lines):
    """Map the line a section header ends on to the cleaned title it announces."""
    titles = {}
    for i in range(len(lines) - 2):
        if _RULE.match(lines[i]) and lines[i + 1].startswith("#") and _RULE.match(lines[i + 2]) \
                and not _RULE.match(lines[i + 1]):
            title = lines[i + 1].lstrip("# ").strip()
            titles[i + 3] = re.sub(r"\s*\(.*\)\s*$", "", title)
    return titles


def parse_text(text, source, path=None):
    label = display_path(path or source.path)
    lines = text.split("\n")
    titles = _section_titles([trim(line) for line in lines])
    binds, blocks = [], []
    submap, open_at = "", 0
    group = source.group or source.name
    for number, raw in enumerate(lines, 1):
        group = titles.get(number, group)
        line = trim(raw)
        if not line or line.startswith("#"):
            continue
        target = _SUBMAP_LINE.match(line)
        if target:
            name = trim(target.group(1))
            if submap:
                blocks.append(SubmapBlock(submap, open_at, closed=name == "reset"))
            submap, open_at = ("", 0) if name == "reset" else (name, number)
            continue
        match = _BIND_LINE.match(line)
        if match:
            rhs = match.group(2)
            fields = tuple(trim(field) for field in rhs.split(","))
            binds.append(Bind(source.name, label, number, line, match.group(1), rhs,
                              fields, submap, group))
    if submap:
        blocks.append(SubmapBlock(submap, open_at, closed=False))
    return Parsed(source, binds, blocks)


def parse_file(source):
    return parse_text(Path(source.path).read_text(encoding="utf-8"), source)


def load_sources(sources):
    """Parse every source that exists; the stock file is mandatory."""
    parsed = []
    for source in sources:
        if not Path(source.path).is_file():
            if source.name == "stock":
                raise FileNotFoundError(source.path)
            continue
        parsed.append(parse_file(source))
    return parsed


def all_binds(parsed):
    return [bind for item in parsed for bind in item.binds]


def assign_names(binds):
    """Stable, unique, -k friendly test names such as workspaces__SUPER_TAB."""
    names, seen = {}, {}
    for bind in binds:
        base = f"{slug(bind.group.lower(), 20)}__{slug(bind.label)}"
        if "r" in bind.flags:
            base += "_release"
        seen[base] = seen.get(base, 0) + 1
        names[bind] = base if seen[base] == 1 else f"{base}_{seen[base]}"
    return names
