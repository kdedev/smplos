"""Prebuilt Yay enrollment; no host package, network or privilege operations."""

import os
from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
BIN = ROOT / "src/shared/bin"
RECIPE = ROOT / "src/shared/pkgbuilds/yay-bin/PKGBUILD"


class YayDeliveryTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.base = Path(self.temp.name)
        self.bin = self.base / "bin"
        self.bin.mkdir()
        self.repo = self.base / "smplos/repo"
        recipe = self.repo / "src/shared/pkgbuilds/yay-bin"
        recipe.mkdir(parents=True)
        (recipe / "PKGBUILD").write_bytes(RECIPE.read_bytes())
        library = self.repo / "src/shared/lib"
        library.mkdir()
        (library / "smplos-pkgbuild.sh").write_text("""
smplos_pkg_version() { echo "${HAVE:-}"; }
smplos_pkgbuild_install() {
    echo "package-binary $*" >> "$CALLS"
    [[ "${PACKAGING_FAIL:-0}" == 0 ]] || return 1
    touch "$READY"
}
""")
        self.calls = self.base / "calls"
        self.ready = self.base / "ready"
        self.env = {
            **os.environ, "PATH": f"{self.bin}:/usr/bin:/bin",
            "SMPLOS_PATH": str(self.repo.parent), "SMPLOS_SYSTEM_UPGRADED": "1",
            "SMPLOS_SUDO_READY": "1", "CALLS": str(self.calls), "READY": str(self.ready),
        }
        self.script = self.base / "ensure-yay"
        self.script.write_text((BIN / "smplos-ensure-yay").read_text().replace(
            '[[ $EUID -ne 0 ]]', 'true'))
        self.mock("sudo", """echo "sudo $*" >> "$CALLS"
[[ "$1" == -n ]] || exit 99
shift
exec "$@"
""")
        self.mock("yay", """echo "yay $*" >> "$CALLS"
[[ "${YAY_WORKS:-0}" == 1 || -f "$READY" ]] || exit 1
if [[ "$*" == "-Q pacman" ]]; then
    [[ "${ALPM_FAIL:-0}" == 0 ]] || exit 1
    echo "pacman 7.1.0-1"
else
    echo "yay v13.0.1"
fi
""")
        self.mock("pacman", """echo "pacman $*" >> "$CALLS"
case "$*" in
    "-Si yay") [[ "${REPO_YAY:-0}" == 1 ]] ;;
    "-Q yay") [[ "${REPO_YAY_INSTALLED:-0}" == 1 ]] ;;
    "-S --noconfirm --ask 4 yay")
        [[ "${INSTALL_FAIL:-0}" == 0 ]] || exit 1
        touch "$READY" ;;
    *) echo FORBIDDEN >> "$CALLS"; exit 99 ;;
esac
""")
        self.mock("vercmp", 'echo "${COMPARISON:-0}"\n')
        for name in ("git", "makepkg", "go", "cargo"):
            self.mock(name, 'echo "FORBIDDEN $0" >> "$CALLS"\nexit 99\n')

    def mock(self, name, body):
        path = self.bin / name
        path.write_text("#!/bin/bash\n" + body)
        path.chmod(0o755)

    def enroll(self, **env):
        result = subprocess.run(["bash", str(self.script)], env={**self.env, **env},
                                capture_output=True, text=True, timeout=5)
        self.assertNotIn("FORBIDDEN", self.calls.read_text() if self.calls.exists() else "")
        return result

    def test_first_enrollment_packages_prebuilt_yay_without_a_compiler_or_new_authentication(self):
        result = self.enroll()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        calls = self.calls.read_text()
        self.assertIn("package-binary", calls)
        self.assertIn("--ask 4", calls)
        self.assertIn("yay -Q pacman", calls)
        self.assertNotIn("sudo -v", calls)

    def test_working_current_binary_is_reused_without_reinstall_or_authentication(self):
        result = self.enroll(HAVE="13.0.1-1", YAY_WORKS="1")
        self.assertEqual(result.returncode, 0, result.stderr)
        calls = self.calls.read_text()
        self.assertNotIn("package-binary", calls)
        self.assertNotIn("sudo ", calls)
        self.assertIn("yay -Q pacman", calls)

    def test_configured_repo_supplies_yay_without_binary_packaging(self):
        result = self.enroll(REPO_YAY="1")
        self.assertEqual(result.returncode, 0, result.stderr)
        calls = self.calls.read_text()
        self.assertIn("sudo -n pacman -S --noconfirm --ask 4 yay", calls)
        self.assertNotIn("package-binary", calls)

    def test_working_repo_yay_is_not_reinstalled(self):
        result = self.enroll(REPO_YAY="1", REPO_YAY_INSTALLED="1", YAY_WORKS="1")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertNotIn("sudo ", self.calls.read_text())
        self.assertNotIn("package-binary", self.calls.read_text())

    def test_missing_base_upgrade_blocks_every_package_or_privilege_action(self):
        result = self.enroll(SMPLOS_SYSTEM_UPGRADED="0")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("completed base upgrade", result.stderr)
        self.assertFalse(self.calls.exists())

    def test_broken_newer_binary_is_not_silently_downgraded(self):
        result = self.enroll(HAVE="14.0.0-1", COMPARISON="1")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("refusing a downgrade", result.stderr)
        self.assertFalse(self.ready.exists())
        self.assertNotIn("package-binary", self.calls.read_text())

    def test_failed_binary_delivery_or_runtime_query_is_explicit(self):
        for failure in ("PACKAGING_FAIL", "ALPM_FAIL", "INSTALL_FAIL"):
            with self.subTest(failure=failure):
                self.calls.write_text("")
                self.ready.unlink(missing_ok=True)
                env = {failure: "1"}
                if failure == "INSTALL_FAIL":
                    env["REPO_YAY"] = "1"
                result = self.enroll(**env)
                self.assertNotEqual(result.returncode, 0)
                self.assertNotIn("pacman -R", self.calls.read_text())

    def test_invalid_version_comparison_is_not_reported_as_success(self):
        result = self.enroll(HAVE="13.0.1-1", COMPARISON="invalid")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Cannot compare installed Yay", result.stderr)
        self.assertNotIn("Yay is ready", result.stdout)

    def test_legacy_hook_does_not_build_or_install_anything(self):
        result = subprocess.run(["bash", str(BIN / "rebuild-paru")], env=self.env,
                                capture_output=True, text=True)
        self.assertEqual(result.returncode, 0)
        self.assertIn("retired", result.stdout)
        self.assertFalse(self.calls.exists())

    def test_recipe_only_installs_upstream_binary_and_checks_actual_alpm_access(self):
        bundle = self.base / "yay_13.0.1_x86_64"
        bundle.mkdir()
        executable = bundle / "yay"
        executable.write_text('#!/bin/bash\necho "recipe-yay $*" >> "$CALLS"\n')
        executable.chmod(0o755)
        for name in ("yay.8", "bash", "zsh", "fish", "en.mo"):
            (bundle / name).write_text(name + "\n")
        destination = self.base / "package"
        result = subprocess.run(
            ["bash", "-c", 'source "$1"; check && package', "fixture", str(RECIPE)],
            env={**self.env, "srcdir": str(self.base), "pkgdir": str(destination)},
            capture_output=True, text=True, timeout=5)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("recipe-yay -Q pacman", self.calls.read_text())
        self.assertEqual((destination / "usr/bin/yay").read_bytes(), executable.read_bytes())
        self.assertTrue(os.access(destination / "usr/bin/yay", os.X_OK))
        self.assertTrue((destination / "usr/share/locale/en/LC_MESSAGES/yay.mo").is_file())
        self.assertNotIn("build()", RECIPE.read_text())
        self.assertNotIn("makedepends=", RECIPE.read_text())


if __name__ == "__main__":
    unittest.main()
