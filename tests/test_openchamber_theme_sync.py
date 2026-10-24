import importlib.machinery
import importlib.util
from http.server import BaseHTTPRequestHandler, HTTPServer
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import threading
import unittest
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "src/shared/bin/theme-set-openchamber"
loader = importlib.machinery.SourceFileLoader("openchamber_theme_sync", str(SCRIPT))
spec = importlib.util.spec_from_loader(loader.name, loader)
sync = importlib.util.module_from_spec(spec)
loader.exec_module(sync)


class Connection:
    status = 200
    reply = {"id": "request-id", "delivered": 1}
    requests = []

    def __init__(self, host, port, timeout):
        self.endpoint = (host, port, timeout)

    def request(self, method, path, body, headers):
        self.requests.append((self.endpoint, method, path, json.loads(body), headers))

    def getresponse(self):
        return self

    def read(self, limit):
        return json.dumps(self.reply).encode()

    def close(self):
        pass


class OpenChamberThemeTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.home = Path(self.tmp.name)
        self.current = self.home / ".config/smplos/current"
        self.current.mkdir(parents=True)
        self.data = self.home / ".config/openchamber"
        self.data.mkdir()
        self.settings = self.data / "settings.json"
        self.saved = b'{"desktopLocalPort":48412,"desktopLocalClientToken":"secret","personal":"keep"}\n'
        self.settings.write_bytes(self.saved)
        self.select("amber")
        Connection.requests = []
        Connection.status = 200
        Connection.reply = {"id": "request-id", "delivered": 1}
        self.patch = patch.object(sync.http.client, "HTTPConnection", Connection)
        self.patch.start()
        self.addCleanup(self.patch.stop)

    def select(self, name):
        (self.current / "theme.name").write_text(name + "\n")
        if (self.current / "theme").exists():
            shutil.rmtree(self.current / "theme")
        shutil.copytree(ROOT / "src/shared/themes" / name, self.current / "theme")

    def test_all_stock_palettes_have_valid_custom_roles_and_exact_background_alpha(self):
        for path in (ROOT / "src/shared/themes").glob("*/colors.toml"):
            with self.subTest(theme=path.parent.name):
                self.select(path.parent.name)
                theme = sync.palette(self.current)
                with path.open("rb") as source:
                    colors = sync.tomllib.load(source)
                self.assertEqual(theme["metadata"]["id"], f"smplos-{path.parent.name}")
                self.assertEqual(theme["colors"]["surface"]["background"], colors["background"])
                self.assertEqual(theme["colors"]["surface"]["foreground"], colors["foreground"])
                self.assertEqual(theme["colors"]["primary"]["base"], colors["accent"])
                self.assertEqual(theme["config"]["backgroundOpacity"], float(colors["app_background_opacity"]))
                self.assertEqual(theme["metadata"]["variant"],
                                 "light" if (path.parent / "light.mode").exists() else "dark")

    def test_publish_uses_loopback_token_and_id_only_and_preserves_settings(self):
        theme = sync.palette(self.current)
        sync.publish(self.home, self.data, theme)
        endpoint, method, path, body, headers = Connection.requests[0]
        self.assertEqual(endpoint, ("127.0.0.1", 48412, 2))
        self.assertEqual((method, path), ("POST", "/api/openchamber/control"))
        self.assertEqual(body, {"action": "theme.set", "input": {"id": "smplos-amber"}})
        self.assertEqual(headers["Authorization"], "Bearer secret")
        self.assertEqual(self.settings.read_bytes(), self.saved)
        target = self.data / "themes/smplos-amber.json"
        self.assertEqual(json.loads(target.read_bytes()), theme)
        backups = list((self.home / ".local/state/smplos/openchamber-theme-backups").rglob("settings.json.*"))
        self.assertEqual(len(backups), 1)
        self.assertEqual(backups[0].read_bytes(), self.saved)
        self.assertEqual(backups[0].stat().st_mode & 0o777, 0o600)
        self.assertEqual(list(target.parent.glob("*.tmp")), [])

    def test_rewrite_backs_up_existing_custom_file_without_duplicate_theme_ids(self):
        target = self.data / "themes/smplos-amber.json"
        target.parent.mkdir()
        previous = b'{"personal":"existing theme"}'
        target.write_bytes(previous)
        for _ in range(2):
            sync.publish(self.home, self.data, sync.palette(self.current))
        self.assertEqual(list(target.parent.glob("*.json")), [target])
        backups = list((self.home / ".local/state/smplos/openchamber-theme-backups").rglob("smplos-amber.json.*"))
        self.assertEqual(len(backups), 1)
        self.assertEqual(backups[0].read_bytes(), previous)

    def test_demo_override_replaces_default_and_never_changes_real_app(self):
        demo = self.home / "demo"
        demo.mkdir()
        (demo / "settings.json").write_text('{"desktopLocalPort":48412}')
        override = self.current.parent / "openchamber-data-dirs"
        override.write_text(f"# isolated demo only\n{demo}\n{demo}\n")
        with patch.dict(sync.os.environ, {}, clear=True):
            self.assertEqual(sync.targets(self.home), [demo])
        sync.publish(self.home, demo, sync.palette(self.current))
        self.assertNotIn("Authorization", Connection.requests[0][-1])
        self.assertFalse((self.data / "themes").exists())
        self.assertEqual(self.settings.read_bytes(), self.saved)
        override.write_text("")
        with self.assertRaises(ValueError):
            sync.targets(self.home)

    def test_missing_app_is_noop_and_explicit_relative_target_is_rejected(self):
        self.settings.unlink()
        with patch.dict(sync.os.environ, {}, clear=True):
            self.assertEqual(sync.targets(self.home), [])
        with patch.dict(sync.os.environ, {"OPENCHAMBER_DATA_DIR": "relative"}, clear=True):
            # A configured target must not silently pass validation when absent.
            with self.assertRaises(ValueError):
                sync.targets(self.home)

    def test_invalid_colors_or_opacity_do_not_publish(self):
        colors = self.current / "theme/colors.toml"
        original = colors.read_text()
        for value in ('"NaN"', '"2"', '"0"', 'true', '""'):
            with self.subTest(value=value):
                colors.write_text(original.replace('app_background_opacity = "1.0"',
                                                   f"app_background_opacity = {value}"))
                with self.assertRaises(ValueError):
                    sync.palette(self.current)
        colors.write_text(original.replace('accent = "#FFA033"', 'accent = "#FFA03380"'))
        with self.assertRaises(ValueError):
            sync.palette(self.current)
        self.assertEqual(Connection.requests, [])

    def test_invalid_endpoint_never_writes_theme_or_calls_server(self):
        for port in (True, "48412", -1, 65536, None):
            self.settings.write_text(json.dumps({"desktopLocalPort": port}))
            with self.assertRaises(ValueError):
                sync.publish(self.home, self.data, sync.palette(self.current))
        self.assertFalse((self.data / "themes").exists())
        self.assertEqual(Connection.requests, [])

    def test_api_rejections_and_zero_delivery_are_errors_not_proof_of_application(self):
        for status in (400, 503, 302):
            Connection.status = status
            with self.assertRaisesRegex(ValueError, f"HTTP {status}"):
                sync.publish(self.home, self.data, sync.palette(self.current))
        Connection.status = 200
        for reply in ({}, {"delivered": 0}, {"delivered": True}):
            Connection.reply = reply
            with self.assertRaisesRegex(ValueError, "did not deliver"):
                sync.publish(self.home, self.data, sync.palette(self.current))

    def test_real_helper_posts_only_to_isolated_server_after_atomic_publication(self):
        demo = self.home / "demo"
        demo.mkdir()
        requests = []

        class Handler(BaseHTTPRequestHandler):
            def do_POST(self):
                body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
                theme = json.loads((demo / "themes/smplos-amber.json").read_bytes())
                requests.append((self.path, body, theme, self.headers.get("Authorization")))
                self.send_response(200)
                self.end_headers()
                self.wfile.write(b'{"id":"request-id","delivered":1}')

            def log_message(self, *_):
                pass

        with HTTPServer(("127.0.0.1", 0), Handler) as server:
            thread = threading.Thread(target=server.serve_forever)
            thread.start()
            try:
                (demo / "settings.json").write_text(json.dumps({"desktopLocalPort": server.server_port}))
                (self.current.parent / "openchamber-data-dirs").write_text(str(demo) + "\n")
                result = subprocess.run(
                    [str(SCRIPT)], env={**os.environ, "HOME": str(self.home)},
                    capture_output=True, text=True, timeout=5,
                )
            finally:
                server.shutdown()
                thread.join()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(len(requests), 1)
        path, body, theme, token = requests[0]
        self.assertEqual(path, "/api/openchamber/control")
        self.assertEqual(body, {"action": "theme.set", "input": {"id": "smplos-amber"}})
        self.assertEqual(theme["colors"]["surface"]["background"], "#000000")
        self.assertEqual(theme["colors"]["surface"]["foreground"], "#FFA033")
        self.assertEqual(theme["config"]["backgroundOpacity"], 1)
        self.assertIsNone(token)
        self.assertEqual(self.settings.read_bytes(), self.saved)
        self.assertFalse((self.data / "themes").exists())


if __name__ == "__main__":
    unittest.main()
