"""Minimal Wayland virtual-keyboard client (stdlib only).

wtype cannot drive compositor binds: Hyprland resolves a bind's keysym from the
raw evdev keycode through its own keymap, and wtype hands out arbitrary keycodes.
This client sends real evdev codes plus an explicit modifier mask instead.
"""
import array
import os
import socket
import struct
import time

MOD_BITS = {"SHIFT": 1, "CAPS": 2, "CTRL": 4, "ALT": 8, "SUPER": 64}

_DISPLAY = 1
_STATE_PRESSED, _STATE_RELEASED = 1, 0


class WaylandError(RuntimeError):
    pass


def _string(s):
    raw = s.encode() + b"\0"
    return struct.pack("<I", len(raw)) + raw + b"\0" * (-len(raw) % 4)


def _read_string(payload, offset):
    (length,) = struct.unpack_from("<I", payload, offset)
    text = payload[offset + 4:offset + 4 + length - 1].decode()
    return text, offset + 4 + length + (-length % 4)


class VirtualKeyboard:
    def __init__(self, socket_path, keymap_text):
        self._sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self._sock.connect(socket_path)
        self._buf = b""
        self._next_id = 2
        registry = self._new_id()
        self._send(_DISPLAY, 1, struct.pack("<I", registry))
        globals_ = {}
        self._handlers = {registry: lambda op, p: self._on_global(globals_, op, p)}
        self.roundtrip()
        missing = {"wl_seat", "zwp_virtual_keyboard_manager_v1"} - globals_.keys()
        if missing:
            raise WaylandError(f"compositor lacks {', '.join(sorted(missing))}")
        seat = self._bind(registry, globals_, "wl_seat", 1)
        manager = self._bind(registry, globals_, "zwp_virtual_keyboard_manager_v1", 1)
        self._kbd = self._new_id()
        self._send(manager, 0, struct.pack("<II", seat, self._kbd))
        self._upload_keymap(keymap_text)
        self.roundtrip()

    def _new_id(self):
        self._next_id += 1
        return self._next_id - 1

    def _send(self, obj, opcode, args=b"", fds=()):
        msg = struct.pack("<II", obj, ((8 + len(args)) << 16) | opcode) + args
        ancillary = [(socket.SOL_SOCKET, socket.SCM_RIGHTS, array.array("i", fds))] if fds else []
        self._sock.sendmsg([msg], ancillary)

    def _bind(self, registry, globals_, interface, version):
        obj = self._new_id()
        name = globals_[interface]
        self._send(registry, 0, struct.pack("<I", name) + _string(interface) + struct.pack("<II", version, obj))
        return obj

    @staticmethod
    def _on_global(globals_, opcode, payload):
        if opcode == 0:
            (name,) = struct.unpack_from("<I", payload)
            interface, _ = _read_string(payload, 4)
            globals_[interface] = name

    def _upload_keymap(self, text):
        data = text.encode() + b"\0"
        fd = os.memfd_create("keymap")
        try:
            os.write(fd, data)
            self._send(self._kbd, 0, struct.pack("<II", 1, len(data)), fds=[fd])
        finally:
            os.close(fd)

    def _messages(self):
        chunk = self._sock.recv(65536)
        if not chunk:
            raise WaylandError("compositor closed the connection")
        self._buf += chunk
        while len(self._buf) >= 8:
            obj, size_op = struct.unpack_from("<II", self._buf)
            size = size_op >> 16
            if len(self._buf) < size:
                break
            payload, self._buf = self._buf[8:size], self._buf[size:]
            yield obj, size_op & 0xFFFF, payload

    def roundtrip(self):
        callback = self._new_id()
        self._send(_DISPLAY, 0, struct.pack("<I", callback))
        while True:
            for obj, opcode, payload in self._messages():
                if obj == _DISPLAY and opcode == 0:
                    _, code = struct.unpack_from("<II", payload)
                    raise WaylandError(f"protocol error {code}: {_read_string(payload, 8)[0]}")
                if obj == callback and opcode == 0:
                    return
                handler = self._handlers.get(obj)
                if handler:
                    handler(opcode, payload)

    def modifiers(self, mask):
        self._send(self._kbd, 2, struct.pack("<IIII", mask, 0, 0, 0))

    def key(self, evdev_code, pressed):
        stamp = int(time.monotonic() * 1000) & 0xFFFFFFFF
        state = _STATE_PRESSED if pressed else _STATE_RELEASED
        self._send(self._kbd, 1, struct.pack("<III", stamp, evdev_code, state))

    def tap(self, evdev_code, mask=0, hold=0.02):
        """Press and release one key with the given modifier mask held."""
        self.modifiers(mask)
        self.key(evdev_code, True)
        self.roundtrip()
        time.sleep(hold)
        self.key(evdev_code, False)
        self.modifiers(0)
        self.roundtrip()

    def close(self):
        self._send(self._kbd, 3)
        self.roundtrip()
        self._sock.close()
