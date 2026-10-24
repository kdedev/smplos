"""libxkbcommon through ctypes, for validating bind keys and finding keycodes."""
import ctypes
import ctypes.util

CASE_INSENSITIVE = 1
TEXT_V1 = 1
EVDEV_OFFSET = 8  # xkb keycode = evdev code + 8


class _RuleNames(ctypes.Structure):
    _fields_ = [(n, ctypes.c_char_p) for n in ("rules", "model", "layout", "variant", "options")]


def _load():
    name = ctypes.util.find_library("xkbcommon")
    if not name:
        return None
    lib = ctypes.CDLL(name)
    c = ctypes
    for fn, args, res in (
        ("xkb_keysym_from_name", [c.c_char_p, c.c_int], c.c_uint32),
        ("xkb_keysym_to_lower", [c.c_uint32], c.c_uint32),
        ("xkb_keysym_to_upper", [c.c_uint32], c.c_uint32),
        ("xkb_context_new", [c.c_int], c.c_void_p),
        ("xkb_keymap_new_from_names", [c.c_void_p, c.POINTER(_RuleNames), c.c_int], c.c_void_p),
        ("xkb_keymap_get_as_string", [c.c_void_p, c.c_int], c.c_void_p),
        ("xkb_keymap_min_keycode", [c.c_void_p], c.c_uint32),
        ("xkb_keymap_max_keycode", [c.c_void_p], c.c_uint32),
        ("xkb_state_new", [c.c_void_p], c.c_void_p),
        ("xkb_state_key_get_one_sym", [c.c_void_p, c.c_uint32], c.c_uint32),
    ):
        getattr(lib, fn).argtypes = args
        getattr(lib, fn).restype = res
    return lib


_LIB = _load()


def available():
    return _LIB is not None


def keysym(name):
    """Keysym for a bind key name, or 0 when libxkbcommon does not know it."""
    return _LIB.xkb_keysym_from_name(name.encode(), CASE_INSENSITIVE)


def canonical(name):
    """Case-folded keysym, which is what Hyprland compares a pressed key against."""
    sym = keysym(name)
    return _LIB.xkb_keysym_to_lower(sym) if sym else 0


class Keymap:
    """The keymap Hyprland resolves binds with (rules evdev, model pc105)."""

    def __init__(self, layout="us", variant="", options=""):
        ctx = _LIB.xkb_context_new(0)
        names = _RuleNames(b"evdev", b"pc105", layout.encode(), variant.encode(), options.encode())
        self._map = _LIB.xkb_keymap_new_from_names(ctx, ctypes.byref(names), 0)
        if not self._map:
            raise ValueError(f"cannot compile keymap for layout {layout!r}")
        self._state = _LIB.xkb_state_new(self._map)

    def text(self):
        ptr = _LIB.xkb_keymap_get_as_string(self._map, TEXT_V1)
        try:
            return ctypes.string_at(ptr).decode()
        finally:
            ctypes.CDLL(None).free(ctypes.c_void_p(ptr))

    def evdev_code(self, key_name):
        """evdev code whose unshifted keysym matches the bind key, or None."""
        sym = keysym(key_name)
        if not sym:
            return None
        wanted = {sym, _LIB.xkb_keysym_to_lower(sym), _LIB.xkb_keysym_to_upper(sym)}
        lo, hi = _LIB.xkb_keymap_min_keycode(self._map), _LIB.xkb_keymap_max_keycode(self._map)
        for code in range(lo, hi + 1):
            if _LIB.xkb_state_key_get_one_sym(self._state, code) in wanted:
                return code - EVDEV_OFFSET
        return None
