"""No package operations, session changes or network calls touch the host."""

from contextlib import redirect_stderr, redirect_stdout
import importlib.machinery
import importlib.util
import io
import json
import os
from pathlib import Path
import re
import shutil
import shlex
import subprocess
import tempfile
import unittest
from unittest import mock

from test_app_delivery import function


ROOT = Path(__file__).resolve().parents[1]
BIN = ROOT / "src/shared/bin"
LOADER = importlib.machinery.SourceFileLoader("update_check", str(BIN / "smplos-update-check"))
SPEC = importlib.util.spec_from_loader(LOADER.name, LOADER)
CHECK = importlib.util.module_from_spec(SPEC)
LOADER.exec_module(CHECK)


class PowerPreflightTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.supplies = Path(self.temp.name)

    def supply(self, name, **attributes):
        directory = self.supplies / name
        directory.mkdir()
        for key, value in attributes.items():
            (directory / key).write_text(str(value) + "\n")

    def check(self):
        script = function(BIN / "smplos-update", "check_update_power")
        return subprocess.run(
            ["bash", "-c", script + "\ncheck_update_power " + shlex.quote(str(self.supplies))],
            text=True, capture_output=True, timeout=5,
        )

    def test_desktop_without_batteries_is_allowed(self):
        self.assertEqual(self.check().returncode, 0)

    def test_peripherals_do_not_turn_a_desktop_into_a_laptop(self):
        self.supply("mouse", type="Battery", scope="Device", status="Discharging")
        self.supply("hidpp_battery_0", type="Battery", status="Discharging")
        self.assertEqual(self.check().returncode, 0)

    def test_full_battery_without_external_power_is_blocked(self):
        self.supply("BAT0", type="Battery", status="Full", capacity=100)
        result = self.check()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("connect the laptop", result.stderr)
        self.assertIn("100%", result.stdout)

    def test_charging_usb_power_and_charge_limits_are_allowed(self):
        self.supply("BAT0", type="Battery", status="Charging", capacity=10)
        self.assertEqual(self.check().returncode, 0)
        (self.supplies / "BAT0/status").write_text("Not charging\n")
        self.supply("USB-C", type="USB", online=1)
        self.assertEqual(self.check().returncode, 0)

    def test_discharging_with_undersized_connected_charger_is_blocked(self):
        self.supply("BAT0", type="Battery", status="Discharging", capacity=90)
        self.supply("AC", type="Mains", online=1)
        self.assertNotEqual(self.check().returncode, 0)

    def test_any_discharging_system_battery_blocks_dual_battery_laptops(self):
        self.supply("BAT0", type="Battery", status="Charging", capacity=30)
        self.supply("BAT1", type="Battery", status="Discharging", capacity=60)
        self.assertNotEqual(self.check().returncode, 0)

    def test_unknown_or_unreadable_battery_state_is_not_silently_accepted(self):
        self.supply("BAT0", type="Battery", status="Unknown")
        self.assertNotEqual(self.check().returncode, 0)
        (self.supplies / "BAT0/status").unlink()
        self.assertNotEqual(self.check().returncode, 0)

    def test_removed_battery_is_ignored(self):
        self.supply("BAT0", type="Battery", present=0)
        self.assertEqual(self.check().returncode, 0)


class InstalledStateTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.base = Path(self.temp.name)
        self.home = self.base / "home"
        self.repo = self.base / "repo"
        self.root = self.base / "system"
        self.calls = []
        self.pending = ""
        self.ldd_error = ""
        self.modinfo_error = False
        self.nvidia_version = "590"
        self.nvidia_vermagic = "6.12-test SMP preempt mod_unload"
        self.initramfs = "usr/lib/modules/6.12-test/kernel/test.ko\n"
        self.packages = {"linux-lts": "6.12-1", "glibc": "2.42-1", "hyprland": "0.56.0-2"}
        self.available = {}
        self.foreign = set()
        self.repository_error = False
        self.extra_repositories = ""
        self.library_packages = ["glibc"]
        self.brave_metadata = "Name : brave-bin\nVersion : 1.83.112-1\n"
        self.brave_lookup_failed = False
        self.brave_runtime_version = "Brave Browser 141.1.83.112\n"
        self.yay_alpm_failed = False
        self.write(self.repo / "src/shared/bin/example", "#!/bin/bash\ntrue\n")
        self.write(self.repo / "src/shared/lib/example.sh", "helper() { :; }\n")
        self.write(self.repo / "src/shared/eww/quick-settings.yuck", "(box)\n")
        self.write(self.repo / "src/shared/eww/quick-settings.scss", ".quick { color: red; }\n")
        self.write(self.repo / "src/shared/eww/eww.yuck", '(include "./quick-settings.yuck")\n')
        self.write(self.repo / "src/shared/eww/eww.scss", '@import "quick-settings";\n')
        self.write(self.repo / "src/shared/eww/scripts/listener.sh", "#!/bin/bash\necho '{}'\n")
        self.write(self.repo / "src/shared/eww/theme-colors.scss", "stock palette\n")
        self.write(self.repo / "src/compositors/hyprland/hypr/windows.lua", "-- stock\n")
        self.write(self.repo / "src/compositors/hyprland/hypr/monitors.conf", "# stock\n")
        self.write(self.repo / "src/shared/themes/test/colors.toml", 'background="#000000"\n')
        for source in self.repo.rglob("*"):
            if not source.is_file():
                continue
            relative = source.relative_to(self.repo)
            text = str(relative)
            if text.startswith("src/shared/bin/"):
                target = self.root / "usr/local/bin" / source.name
            elif text.startswith("src/shared/lib/"):
                target = self.root / "usr/local/lib/smplos" / source.name
            elif text.startswith("src/shared/eww/"):
                target = self.home / ".config/eww" / source.relative_to(self.repo / "src/shared/eww")
            elif text.startswith("src/shared/themes/"):
                target = self.home / ".local/share/smplos/themes" / source.relative_to(self.repo / "src/shared/themes")
            else:
                target = self.home / ".config/hypr" / source.name
            self.write(target, source.read_text())
        self.write(self.home / ".config/eww/theme-colors.scss", "custom generated palette\n")
        self.write(self.home / ".config/hypr/monitors.conf", "user-owned monitor setup\n")
        for name in (*CHECK.APPS, "st-wl", "micro"):
            self.write(self.root / "usr/local/bin" / name, b"\x7fELF" + name.encode())
        for name in ("nemo", "Hyprland", "eww", "grafium"):
            self.write(self.root / "usr/bin" / name, b"\x7fELF" + name.encode())
        for name in ("nemo-document-renderer", "nemo-archive-create-worker"):
            self.write(self.root / "usr/libexec" / name, b"\x7fELF" + name.encode())
        receipt = "".join(
            f"{CHECK.digest(self.root / 'usr/local/bin' / name)}  {self.root / 'usr/local/bin' / name}\n"
            for name in CHECK.APPS
        )
        self.write(self.home / ".local/state/smplos/app-versions/smpl-apps.sha256", receipt)
        self.write(self.home / ".local/state/smplos/app-versions/smpl-apps", "v0.8.28\n")
        for component, name in (("st-smpl", "st-wl"), ("micro-smpl", "micro")):
            state = self.home / ".local/state/smplos/app-versions"
            binary = self.root / "usr/local/bin" / name
            self.write(state / component, "v1.0\n")
            self.write(state / f"{component}.sha256", f"{CHECK.digest(binary)}  {binary}\n")
        self.write(self.root / "boot/vmlinuz-linux-lts", "kernel\n")
        self.write(self.root / "boot/initramfs-linux-lts.img", "initramfs\n")
        self.write(self.root / "usr/lib/modules/6.12-test/vmlinuz", "kernel\n")
        self.write(self.root / "usr/lib/modules/6.12-test/pkgbase", "linux-lts\n")

    def write(self, path, content):
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(content if isinstance(content, bytes) else content.encode())
        path.chmod(0o755)

    def runner(self, *args):
        self.calls.append(args)
        status, out, err = 0, "", ""
        if args == ("pacman", "-Q", "brave-bin"):
            if "brave-bin" in self.packages:
                out = f"brave-bin {self.packages['brave-bin']}\n"
            else:
                status = 1
        elif args[:2] == ("pacman", "-Q"):
            out = "".join(f"{key} {value}\n" for key, value in self.packages.items())
        elif args[:2] == ("pacman", "-Qu"):
            out = self.pending
            status = 0 if out else 1
        elif args[:2] == ("pacman", "-Sl"):
            out = "".join(f"core {name} {self.available.get(name, version)}\n"
                          for name, version in self.packages.items() if name not in self.foreign)
            out += self.extra_repositories
            status = int(self.repository_error)
        elif args[:2] == ("pacman", "-Qoq"):
            out = "\n".join(self.library_packages) + "\n"
        elif args[:2] == ("pacman", "-Qql"):
            out = ("/usr/bin/brave\n/opt/brave-bin/brave\n" if args[2] == "brave-bin"
                   else "/usr/lib/modules/6.12-test/pkgbase\n")
        elif args == ("yay", "-Si", "--aur", "brave-bin"):
            out, status = self.brave_metadata, int(self.brave_lookup_failed)
        elif len(args) == 2 and args[0].endswith("/brave") and args[1] == "--version":
            out = self.brave_runtime_version
        elif len(args) == 3 and args[0].endswith("/yay") and args[1:] == ("-Q", "pacman"):
            out, status = "pacman 7.1.0-1\n", int(self.yay_alpm_failed)
        elif args[0] == "git":
            out = "f" * 40
        elif args[0] == "ldd":
            if self.ldd_error and args[1].endswith(self.ldd_error):
                status, out = 1, "libhyprutils.so.99 => not found\n"
            else:
                out = "libc.so.6 => /usr/lib/libc.so.6\n"
        elif args[0] == "modinfo":
            status = int(self.modinfo_error)
            out = {"filename": "/usr/lib/modules/6.12-test/updates/dkms/nvidia.ko.zst",
                   "version": self.nvidia_version, "vermagic": self.nvidia_vermagic}[args[4]]
        elif args[0] == "sudo":
            out = self.initramfs
        elif args[0] == "vercmp":
            left, right = (tuple(int(part) for part in re.findall(r"\d+", value)) for value in args[1:])
            out = str((left > right) - (left < right)) + "\n"
        return subprocess.CompletedProcess(args, status, out, err)

    def audit(self):
        audit = CHECK.Audit(self.home, self.repo, self.root, self.runner)
        with mock.patch.dict(os.environ, {"DISPLAY": "", "WAYLAND_DISPLAY": "",
                                         "HYPRLAND_INSTANCE_SIGNATURE": ""}):
            audit.snapshot()
            return audit.verify("full")

    def test_complete_install_passes_and_preserves_user_config_and_generated_palette(self):
        report = self.audit()
        self.assertEqual(report["errors"], [])
        self.assertEqual(report["app_versions"]["smpl-apps"], "v0.8.28")
        self.assertIn(("pacman", "-Dk"), self.calls)
        critical = next(args for args in self.calls if args[:2] == ("pacman", "-Qk"))
        self.assertIn("glibc", critical)
        self.assertIn("hyprland", critical)
        self.assertIn("linux-lts", critical)

    def test_missing_fragments_and_stale_shared_helpers_fail(self):
        (self.home / ".config/eww/quick-settings.scss").unlink()
        self.write(self.root / "usr/local/lib/smplos/example.sh", "old helper\n")
        errors = "\n".join(self.audit()["errors"])
        self.assertIn("quick-settings.scss", errors)
        self.assertIn("example.sh", errors)

    def test_native_receipt_detects_old_but_valid_elf_not_just_missing_binaries(self):
        self.write(self.root / "usr/local/bin/settings", b"\x7fELFold-settings")
        self.assertIn("differs from its release receipt", "\n".join(self.audit()["errors"]))

    def test_shared_library_abi_failure_is_explicit(self):
        self.ldd_error = "Hyprland"
        self.assertIn("libhyprutils.so.99", "\n".join(self.audit()["errors"]))

    def test_ignored_or_remaining_repository_updates_cannot_report_success(self):
        self.pending = "glibc 2.41-1 -> 2.42-1\n"
        self.assertIn("Repository updates remain", "\n".join(self.audit()["errors"]))

    def test_missing_kernel_or_wrong_initramfs_is_reported_before_reboot(self):
        (self.root / "boot/vmlinuz-linux-lts").unlink()
        self.initramfs = "usr/lib/modules/old-kernel/kernel/test.ko\n"
        errors = "\n".join(self.audit()["errors"])
        self.assertIn("Missing/empty boot artifact", errors)
        self.assertIn("Initramfs does not contain", errors)

    def test_nvidia_module_must_exist_for_the_installed_kernel_not_just_running_kernel(self):
        self.packages["nvidia-open-dkms"] = "590-1"
        self.modinfo_error = True
        self.assertIn("NVIDIA module missing for installed kernel 6.12-test", "\n".join(self.audit()["errors"]))

    def test_retained_running_kernel_modules_do_not_fail_the_new_installed_kernel_check(self):
        self.write(self.root / "usr/lib/modules/old-retained/pkgbase", "linux-lts\n")
        self.packages["nvidia-open-dkms"] = "590-1"
        self.assertEqual(self.audit()["errors"], [])
        self.assertIn(("modinfo", "-k", "6.12-test", "-F", "filename", "nvidia"), self.calls)
        self.assertFalse(any("old-retained" in arg for args in self.calls for arg in args))

    def test_nvidia_module_must_match_both_updated_driver_and_kernel(self):
        self.packages["nvidia-open-dkms"] = "590-1"
        self.nvidia_version = "580"
        self.nvidia_vermagic = "old-kernel SMP"
        errors = "\n".join(self.audit()["errors"])
        self.assertIn("differs from its installed driver package", errors)
        self.assertIn("not built for installed kernel", errors)

    def test_nonexecutable_native_binary_is_not_reported_healthy(self):
        (self.root / "usr/local/bin/settings").chmod(0o644)
        self.assertIn("Application is not executable", "\n".join(self.audit()["errors"]))

    def test_report_is_atomic_persistent_json(self):
        destination = self.base / "reports/after.json"
        report = self.audit()
        CHECK.save_report(destination, report)
        self.assertEqual(json.loads(destination.read_text()), report)
        self.assertEqual(list(destination.parent.iterdir()), [destination])

    def brave_record(self):
        audit = CHECK.Audit(self.home, self.repo, self.root, self.runner)
        audit.capture_brave_target()
        path = self.base / "brave-target.json"
        CHECK.save_report(path, audit.report)
        return path

    def test_brave_target_is_recorded_before_updating_and_equal_or_newer_is_accepted(self):
        self.packages["brave-bin"] = "1.82.100-1"
        record = self.brave_record()
        captured = json.loads(record.read_text())
        self.assertEqual(captured["errors"], [])
        self.assertEqual(captured["brave"]["before"], "1.82.100-1")
        self.assertEqual(captured["brave"]["target"], "1.83.112-1")
        for installed, status in (("1.83.112-1", "current"), ("1.83.112-2", "newer-local"),
                                  ("1.84.0-1", "newer-local")):
            with self.subTest(installed=installed):
                self.packages["brave-bin"] = installed
                audit = CHECK.Audit(self.home, self.repo, self.root, self.runner)
                audit.snapshot()
                audit.brave_result(record)
                self.assertEqual(audit.report["errors"], [])
                self.assertEqual(audit.report["brave"]["status"], status)
                self.assertEqual(audit.report["brave"]["installed"], installed)

    def test_older_or_missing_brave_cannot_meet_the_recorded_update_target(self):
        record = self.brave_record()
        for installed in ("1.82.100-1", "1.83.112-0", None):
            with self.subTest(installed=installed):
                if installed:
                    self.packages["brave-bin"] = installed
                else:
                    self.packages.pop("brave-bin", None)
                audit = CHECK.Audit(self.home, self.repo, self.root, self.runner)
                audit.snapshot()
                audit.brave_result(record)
                self.assertTrue(audit.report["errors"])
                self.assertIn(audit.report["brave"]["status"], ("pending", "missing"))

    def test_failed_or_malformed_brave_metadata_never_creates_a_successful_target(self):
        for failed, metadata in ((True, self.brave_metadata),
                                 (False, "Name : unrelated-browser\nVersion : 1.83.112-1\n"),
                                 (False, "Name : brave-bin\nVersion : unknown\n")):
            with self.subTest(metadata=metadata, failed=failed):
                self.brave_lookup_failed, self.brave_metadata = failed, metadata
                record = self.brave_record()
                self.assertTrue(json.loads(record.read_text())["errors"])
                audit = CHECK.Audit(self.home, self.repo, self.root, self.runner)
                audit.snapshot()
                with self.assertRaises(ValueError):
                    audit.brave_result(record)

    def test_brave_browser_elf_and_actual_version_are_checked_not_just_the_package_marker(self):
        self.packages["brave-bin"] = "1.83.112-1"
        self.write(self.root / "usr/bin/brave", "#!/bin/bash\nexec /opt/brave-bin/brave \"$@\"\n")
        binary = self.root / "opt/brave-bin/brave"
        self.write(binary, b"\x7fELFbrave")
        for output in ("Brave Browser 141.1.83.112\n", "Brave Browser 1.83.112\n"):
            self.brave_runtime_version = output
            report = self.audit()
            self.assertEqual(report["errors"], [])
            self.assertEqual(report["brave_runtime"]["version"], "1.83.112")
            self.assertIn("brave-bin", report["critical_packages"])
        self.brave_runtime_version = "Brave Browser 140.1.82.100\n"
        self.assertIn("actual browser version does not match", "\n".join(self.audit()["errors"]))
        self.brave_runtime_version = "unrecognized output\n"
        self.assertIn("Cannot parse Brave's runtime version", "\n".join(self.audit()["errors"]))
        self.brave_runtime_version = "Brave Browser 1.83.112\n"
        self.ldd_error = "brave"
        self.assertIn("Missing/incompatible shared libraries", "\n".join(self.audit()["errors"]))
        binary.unlink()
        self.assertIn("no installed browser ELF binary", "\n".join(self.audit()["errors"]))

    def test_apps_mode_does_not_claim_to_deploy_desktop_configs(self):
        (self.home / ".config/eww/quick-settings.scss").unlink()
        self.write(self.home / ".config/hypr/windows.lua", "-- older deployed config\n")
        audit = CHECK.Audit(self.home, self.repo, self.root, self.runner)
        audit.snapshot()
        self.assertEqual(audit.verify("apps")["errors"], [])

    def test_bootstrap_checks_every_command_and_helper_without_requiring_new_native_apps(self):
        (self.root / "usr/local/bin/settings").unlink()
        (self.home / ".config/eww/quick-settings.scss").unlink()
        audit = CHECK.Audit(self.home, self.repo, self.root, self.runner)
        self.assertEqual(audit.verify("bootstrap")["errors"], [])
        self.assertEqual(self.calls, [])
        self.write(self.root / "usr/local/lib/smplos/example.sh", "old helper\n")
        self.assertIn("example.sh", "\n".join(audit.verify("bootstrap")["errors"]))

    def test_bootstrap_rejects_a_byte_current_but_nonexecutable_command(self):
        (self.root / "usr/local/bin/example").chmod(0o644)
        audit = CHECK.Audit(self.home, self.repo, self.root, self.runner)
        self.assertIn("Managed executable is not executable", "\n".join(audit.verify("bootstrap")["errors"]))

    def test_yay_must_access_lib_alpm_not_merely_have_a_valid_elf(self):
        self.packages["yay-bin"] = "13.0.1-1"
        self.write(self.root / "usr/bin/yay", b"\x7fELFyay")
        self.assertEqual(self.audit()["errors"], [])
        self.yay_alpm_failed = True
        self.assertIn("Yay cannot read the installed pacman/libalpm", "\n".join(self.audit()["errors"]))

    def test_non_hyprland_system_does_not_require_hyprland_config_or_binary(self):
        self.packages.pop("hyprland")
        (self.root / "usr/bin/Hyprland").unlink()
        (self.home / ".config/hypr/windows.lua").unlink()
        self.assertEqual(self.audit()["errors"], [])

    def test_malformed_receipt_still_collects_package_and_boot_diagnostics(self):
        self.write(self.home / ".local/state/smplos/app-versions/smpl-apps.sha256", "broken\n")
        self.pending = "glibc 2.41 -> 2.42\n"
        errors = "\n".join(self.audit()["errors"])
        self.assertIn("Malformed smpl-apps", errors)
        self.assertIn("Repository updates remain", errors)

    def test_every_package_is_compared_not_just_the_critical_shortlist(self):
        self.packages["unlisted-library"] = "1.0-1"
        self.available["unlisted-library"] = "2.0-1"
        report = self.audit()
        self.assertEqual(report["package_status"]["unlisted-library"],
                         {"installed": "1.0-1", "available": "2.0-1",
                          "repository": "core", "status": "pending"})
        self.assertIn("Package was not updated: unlisted-library", "\n".join(report["errors"]))

    def test_no_change_can_mean_current_and_native_version_changes_are_recorded(self):
        audit = CHECK.Audit(self.home, self.repo, self.root, self.runner)
        audit.snapshot()
        audit.verify("full")
        audit.changes({"packages": {**self.packages, "glibc": "2.41-1"},
                       "app_versions": {"smpl-apps": "v0.8.22"}})
        self.assertEqual(audit.report["package_status"]["glibc"]["before"], "2.41-1")
        self.assertTrue(audit.report["package_status"]["glibc"]["changed"])
        self.assertEqual(audit.report["package_status"]["linux-lts"]["status"], "current")
        self.assertFalse(audit.report["package_status"]["linux-lts"]["changed"])
        self.assertEqual(audit.report["app_version_changes"]["smpl-apps"],
                         {"before": "v0.8.22", "after": "v0.8.28"})

    def test_repository_priority_wins_over_a_newer_lower_priority_package(self):
        self.extra_repositories = "extra glibc 99.0-1\n"
        self.assertEqual(self.audit()["package_status"]["glibc"]["available"], "2.42-1")

    def test_local_packages_and_newer_local_versions_are_not_claimed_updated(self):
        self.foreign.add("glibc")
        self.available["hyprland"] = "0.56.0-1"
        report = self.audit()
        self.assertEqual(report["package_status"]["glibc"]["status"], "not-in-repositories")
        self.assertEqual(report["package_status"]["hyprland"]["status"], "newer-local")
        self.assertTrue(any("cannot certify their latest release" in text for text in report["warnings"]))

    def test_missing_repository_inventory_is_an_error_not_current_or_foreign(self):
        self.repository_error = True
        report = self.audit()
        self.assertFalse(report["repository_comparison_valid"])
        self.assertIn("Cannot compare installed packages", "\n".join(report["errors"]))
        self.assertEqual(report["package_status"]["glibc"]["status"], "unknown")

    def test_update_tool_runtime_and_dynamically_discovered_libraries_are_checked(self):
        self.packages.update({"pacman": "7.0-1", "extra-runtime-lib": "3.1-1"})
        self.library_packages.append("extra-runtime-lib")
        self.write(self.root / "usr/bin/pacman", b"\x7fELFpacman")
        self.ldd_error = "/pacman"
        report = self.audit()
        self.assertIn("libhyprutils.so.99", "\n".join(report["errors"]))
        self.assertIn("extra-runtime-lib", report["critical_packages"])
        self.assertEqual(report["runtime_library_packages"]["extra-runtime-lib"], "3.1-1")
        checked = next(call for call in self.calls if call[:2] == ("pacman", "-Qk"))
        self.assertIn("pacman", checked)
        self.assertIn("extra-runtime-lib", checked)

    def test_delivered_optional_binaries_are_receipted_and_linkage_checked(self):
        binary = self.root / "usr/local/bin/xrctl"
        self.write(binary, b"\x7fELFxrctl")
        state = self.home / ".local/state/smplos/app-versions/smpl-apps.sha256"
        state.write_text(state.read_text() + f"{CHECK.digest(binary)}  {binary}\n")
        self.assertEqual(self.audit()["errors"], [])
        self.write(binary, b"\x7fELFold-xrctl")
        report = self.audit()
        self.assertEqual(report["native_files"][str(binary)]["status"], "mismatch")
        self.assertIn(str(binary), report["linkage"])
        binary.unlink()
        report = self.audit()
        self.assertIn("Missing application", "\n".join(report["errors"]))
        self.assertEqual(report["native_files"][str(binary)]["status"], "mismatch")

    def test_native_target_is_checked_independently_of_a_success_result(self):
        self.packages.update({"nemo-smpl": "1.0-1", "grafium-bin": "1.0-1"})
        components = {
            name: {"status": "current", "target": "v1.0"} for name in CHECK.RELEASE_COMPONENTS
        }
        components["smpl-apps"]["target"] = "v0.8.29"
        path = self.base / "apps.json"
        path.write_text(json.dumps({"components": components}))
        audit = CHECK.Audit(self.home, self.repo, self.root, self.runner)
        audit.snapshot()
        audit.release_results(path)
        self.assertIn("did not reach release target v0.8.29", "\n".join(audit.report["errors"]))
        components["smpl-apps"] = {"status": "failed", "target": None}
        path.write_text(json.dumps({"components": components}))
        audit.release_results(path)
        self.assertIn("native release update was not completed", "\n".join(audit.report["errors"]))

    def test_command_persists_full_failed_comparison_and_prints_end_summary(self):
        self.available["glibc"] = "2.43-1"
        before = self.base / "before.json"
        output = self.base / "after.json"
        before.write_text(json.dumps({"packages": {**self.packages, "glibc": "2.41-1"},
                                      "app_versions": {"smpl-apps": "v0.8.22"}}))
        audit = CHECK.Audit(self.home, self.repo, self.root, self.runner)
        stdout, stderr = io.StringIO(), io.StringIO()
        with mock.patch.object(CHECK, "Audit", return_value=audit), mock.patch.object(
            CHECK.sys, "argv", ["smplos-update-check", "--before", str(before),
                               "--apps-report", str(self.base / "not-written.json"),
                               "--output", str(output)]
        ), redirect_stdout(stdout), redirect_stderr(stderr):
            status = CHECK.main()
        self.assertEqual(status, 1)
        report = json.loads(output.read_text())
        self.assertEqual(report["package_status"]["glibc"]["status"], "pending")
        self.assertEqual(report["package_changes"]["glibc"], {"before": "2.41-1", "after": "2.42-1"})
        self.assertIn("1 still pending", stdout.getvalue())
        self.assertIn("target=2.43-1; pending", stdout.getvalue())
        self.assertIn("Native release results unavailable", stderr.getvalue())
        self.assertIn("Verification report", stdout.getvalue())


class ManagedConfigDeliveryTests(unittest.TestCase):
    def test_fragments_and_scripts_publish_before_entrypoints_without_touching_palette(self):
        with tempfile.TemporaryDirectory() as temporary:
            base = Path(temporary)
            repo, home = base / "repo", base / "home"
            source = repo / "src/shared/eww"
            source.mkdir(parents=True)
            target = home / ".config/eww"
            target.mkdir(parents=True)
            (target / "theme-colors.scss").write_text("custom generated palette\n")
            for name in ("quick-settings.yuck", "quick-settings.scss", "notification-hub.yuck",
                         "notification-hub.scss", "eww.yuck", "eww.scss", "theme-colors.scss"):
                (source / name).write_text("new " + name)
            listener = source / "scripts/listener.sh"
            listener.parent.mkdir()
            listener.write_text("#!/bin/bash\ntrue\n")
            log = base / "copies"
            script = function(BIN / "smplos-os-update", "copy_user_config")
            script += "\n" + function(BIN / "smplos-os-update", "sync_configs")
            script += """
as_invoker() {
    if [[ "$1" == install ]]; then echo "${@: -1}" >> "$COPIES"; fi
    "$@"
}
header() { :; }; ok() { :; }; die() { echo "$*" >&2; exit 1; }
sync_configs
"""
            result = subprocess.run(["bash", "-eu", "-c", script], capture_output=True, text=True,
                                    env={**os.environ, "SMPLOS_REPO": str(repo), "HOME": str(home),
                                         "COPIES": str(log)}, timeout=10)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual((target / "theme-colors.scss").read_text(), "custom generated palette\n")
            order = log.read_text()
            self.assertLess(order.index("quick-settings.yuck."), order.index("eww.yuck."))
            self.assertLess(order.index("notification-hub.scss."), order.index("eww.scss."))
            self.assertTrue(os.access(target / "scripts/listener.sh", os.X_OK))
            (target / "scripts/listener.sh").chmod(0o644)
            result = subprocess.run(["bash", "-eu", "-c", script], capture_output=True, text=True,
                                    env={**os.environ, "SMPLOS_REPO": str(repo), "HOME": str(home),
                                         "COPIES": str(log)}, timeout=10)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertTrue(os.access(target / "scripts/listener.sh", os.X_OK))


class UpdaterFlowTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.base = Path(self.temp.name)
        self.home = self.base / "home"
        self.bin = self.base / "bin"
        self.bin.mkdir()
        self.calls = self.base / "calls"
        self.log = self.base / "update.log"
        self.repo = self.home / ".local/share/smplos/repo"
        (self.repo / "migrations").mkdir(parents=True)
        policy = self.repo / "src/shared/update-policy"
        policy.mkdir(parents=True)
        (policy / "critical-bundle.conf").write_text('HYPRLAND_SERIES="0.56"\n')
        for name in ("20260805-100050-clear-ignorepkg-hyprland-eww.sh",
                     "20260805-100060-remove-dropped-lib32-gstreamer.sh"):
            (self.repo / "migrations" / name).write_text("exit 0\n")
        self.env = {
            "HOME": str(self.home), "PATH": f"{self.bin}:/usr/bin:/bin", "LC_ALL": "C",
            "CALLS": str(self.calls), "SMPLOS_UPDATE_LOG_FILE": str(self.log),
        }
        self.source = (BIN / "smplos-update").read_text()
        self.source = self.source.replace("/usr/local/sbin", str(self.bin))
        self.source = self.source.replace("/usr/local/bin", str(self.bin))
        self.mock("sudo", """echo "sudo $*" >> "$CALLS"
[[ "$1" != -n ]] || shift
case "$*" in
    -v|"rm -f /etc/sudoers.d/99-smplos-update-transient"|"rm -f /etc/pacman.d/hooks/90-rebuild-paru.hook") exit 0 ;;
    pacman*|flatpak*) exec "$@" ;;
    *) echo "FORBIDDEN: sudo $*" >&2; exit 99 ;;
esac
""")
        self.mock("pacman", """echo "pacman $*" >> "$CALLS"
case "$1" in
    -Si) echo 'Version : 0.56.2-1' ;;
    -Q) echo 'hyprland 0.56.2-1' ;;
    -Qq)
        [[ "${PARU_INSTALLED:-0}" == 1 && "${2:-}" == paru-bin ]] || exit 1 ;;
    -R) exit "${PARU_REMOVE_FAIL:-0}" ;;
    -Su) exit "${BASE_FAIL:-0}" ;;
esac
""")
        self.mock("pacman-conf", '[[ "${CUSTOM_HOLD:-0}" != 1 ]] || echo "glibc"\n')
        self.mock("smplos-os-update", """echo "os-update $*" >> "$CALLS"
case "$1" in
    --deploy) exit "${DEPLOY_FAIL:-0}" ;;
    --scripts) exit "${BOOTSTRAP_FAIL:-0}" ;;
    --finalize) exit "${FINALIZE_FAIL:-0}" ;;
esac
""")
        self.mock("smplos-update-apps", 'echo native-apps >> "$CALLS"\nexit "${APPS_FAIL:-0}"\n')
        self.mock("smplos-update-check", """echo "check $*" >> "$CALLS"
snapshot=0
[[ "$*" != *--snapshot* ]] || snapshot=1
capture_brave=0
[[ "$*" != *--capture-brave-target* ]] || capture_brave=1
while [[ $# -gt 0 ]]; do
    if [[ "$1" == --output ]]; then echo '{"packages":{}}' > "$2"; break; fi
    shift
done
[[ "$snapshot" != 1 ]] || exit 0
[[ "$capture_brave" != 1 ]] || exit "${BRAVE_TARGET_FAIL:-0}"
exit "${VERIFY_FAIL:-0}"
""")
        source_bin = self.repo / "src/shared/bin"
        source_bin.mkdir()
        (source_bin / "smplos-os-update").write_text(
            "SMPLOS_UPDATE_PROTOCOL=1\n" + (self.bin / "smplos-os-update").read_text())
        (source_bin / "smplos-update-check").write_text("""import os, pathlib, sys
pathlib.Path(sys.argv[sys.argv.index("--output") + 1]).write_text('{"errors":[]}')
sys.exit(int(os.environ.get("BOOTSTRAP_VERIFY_FAIL", "0")))
""")
        self.mock("smplos-ensure-yay", 'echo enroll-yay >> "$CALLS"\nexit "${YAY_ENROLL_FAIL:-0}"\n')
        self.mock("yay", 'echo "yay $*" >> "$CALLS"\nexit "${AUR_FAIL:-0}"\n')
        self.mock("curl", "exit 0\n")
        self.mock("flatpak", """echo "flatpak $*" >> "$CALLS"
[[ "${FLATPAK_FAIL:-0}" != 1 || "$*" != *"update"* ]] || exit 1
if [[ "$1" == list && "${FLATPAK_INSTALLED:-0}" == 1 ]]; then
    echo 'runtime/org.example.Platform/x86_64/stable'
fi
""")
        self.mock("hyprctl", "exit 0\n")
        self.mock("sleep", "exit 0\n")

    def mock(self, name, text):
        path = self.bin / name
        path.write_text("#!/bin/bash\n" + text)
        path.chmod(0o755)

    def run_body(self, mode="full", **variables):
        body = self.source.split('cat >> "$UPDATE_SCRIPT" << \'EOF\'', 1)[1].split("\n", 1)[1].split("\nEOF\n", 1)[0]
        script = self.base / "run-update.sh"
        script.write_text(function(BIN / "smplos-update", "stop_sudo_keepalive") +
                          f'\nUPDATE_MODE={mode}\n' + """
power_checks=0
check_update_power() {
    power_checks=$((power_checks + 1))
    [[ "${POWER_FAIL:-0}" == 0 && $power_checks -le "${POWER_FAIL_AFTER:-999}" ]]
}
""" + body)
        result = subprocess.run(["bash", str(script)], text=True, capture_output=True,
                                env={**self.env, **variables}, timeout=10)
        self.assertNotIn("FORBIDDEN", result.stdout + result.stderr)
        return result

    def test_successful_full_flow_upgrades_base_before_deploy_and_native_and_finalizes_last(self):
        result = self.run_body()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        calls = self.calls.read_text()
        self.assertLess(calls.index("pacman -Su"), calls.index("os-update --deploy"))
        self.assertLess(calls.index("os-update --deploy"), calls.index("native-apps"))
        self.assertLess(calls.index("check --before"), calls.index("os-update --finalize"))
        state = (self.home / ".local/state/smplos/update/status").read_text()
        self.assertIn("status=complete", state)
        self.assertTrue(self.log.is_file())
        self.assertTrue(Path(str(self.log) + ".before.json").is_file())
        self.assertTrue(Path(str(self.log) + ".after.json").is_file())

    def test_failed_base_skips_configs_native_and_aur_but_still_writes_report(self):
        result = self.run_body(BASE_FAIL="1")
        self.assertNotEqual(result.returncode, 0, result.stdout)
        calls = self.calls.read_text()
        for forbidden in ("--deploy", "native-apps", "yay ", "enroll-yay", "--finalize"):
            self.assertNotIn(forbidden, calls)
        self.assertIn("check --before", calls)
        self.assertNotIn("Update complete!", result.stdout)
        self.assertIn("status=failed", (self.home / ".local/state/smplos/update/status").read_text())

    def test_deployment_native_and_audit_failures_never_publish_os_version_or_success(self):
        for failure in ("DEPLOY_FAIL", "APPS_FAIL", "VERIFY_FAIL", "FINALIZE_FAIL"):
            with self.subTest(failure=failure):
                self.calls.write_text("")
                result = self.run_body(**{failure: "1"})
                self.assertNotEqual(result.returncode, 0, result.stdout)
                self.assertNotIn("Update complete!", result.stdout)
                if failure != "FINALIZE_FAIL":
                    self.assertNotIn("--finalize", self.calls.read_text())
                self.assertIn("status=failed", (self.home / ".local/state/smplos/update/status").read_text())

    def test_apps_mode_never_performs_an_official_or_aur_host_upgrade(self):
        result = self.run_body(mode="apps")
        self.assertEqual(result.returncode, 0, result.stdout)
        calls = self.calls.read_text()
        self.assertIn("native-apps", calls)
        self.assertNotIn("pacman -S", calls)
        self.assertNotIn("yay ", calls)
        self.assertNotIn("enroll-yay", calls)
        self.assertNotIn("--deploy", calls)
        self.assertNotIn("--finalize", calls)
        self.assertNotIn("--capture-brave-target", calls)
        self.assertNotIn("--brave-report", calls)

    def test_brave_target_capture_precedes_aur_upgrade_and_final_verification_requires_that_record(self):
        result = self.run_body()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        calls = self.calls.read_text()
        self.assertLess(calls.index("enroll-yay"), calls.index("--capture-brave-target"))
        self.assertLess(calls.index("--capture-brave-target"), calls.index("yay -Sua"))
        self.assertLess(calls.index("90-rebuild-paru.hook"), calls.index("pacman -Su"))
        self.assertIn(f"--brave-report {self.log}.brave.json", calls)
        self.calls.write_text("")
        result = self.run_body(BRAVE_TARGET_FAIL="1")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("yay -Sua", self.calls.read_text())
        self.assertNotIn("--finalize", self.calls.read_text())
        self.assertNotIn("Update complete!", result.stdout)

    def test_failed_yay_enrollment_skips_aur_and_never_finalizes(self):
        result = self.run_body(YAY_ENROLL_FAIL="1")
        self.assertNotEqual(result.returncode, 0)
        calls = self.calls.read_text()
        self.assertNotIn("yay -Sua", calls)
        self.assertNotIn("--capture-brave-target", calls)
        self.assertIn("--brave-report", calls)
        self.assertNotIn("--finalize", calls)

    def test_retired_paru_cannot_block_base_with_old_libalpm_dependencies(self):
        result = self.run_body(PARU_INSTALLED="1")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        calls = self.calls.read_text()
        self.assertLess(calls.index("pacman -R --noconfirm paru-bin"), calls.index("pacman -Su"))
        self.assertNotIn("-Rns", calls)
        self.calls.write_text("")
        result = self.run_body(PARU_INSTALLED="1", PARU_REMOVE_FAIL="1")
        self.assertNotEqual(result.returncode, 0)
        calls = self.calls.read_text()
        self.assertNotIn("pacman -Su", calls)
        self.assertNotIn("--deploy", calls)
        self.assertNotIn("--finalize", calls)

    def test_ac_removed_after_package_transaction_stops_before_new_deployment_transactions(self):
        result = self.run_body(POWER_FAIL_AFTER="2")
        self.assertNotEqual(result.returncode, 0)
        calls = self.calls.read_text()
        self.assertIn("pacman -Su", calls)
        self.assertNotIn("--deploy", calls)
        self.assertNotIn("native-apps", calls)
        self.assertNotIn("yay ", calls)
        self.assertNotIn("--finalize", calls)
        self.assertIn("check --before", calls)

    def test_custom_package_holds_stop_before_any_package_transaction(self):
        result = self.run_body(CUSTOM_HOLD="1")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("custom IgnorePkg", result.stdout)
        self.assertNotIn("pacman -S", self.calls.read_text())
        self.assertNotIn("native-apps", self.calls.read_text())

    def test_unexpected_exit_persists_interrupted_stage(self):
        self.mock("smplos-update-check", 'kill -TERM "$PPID"\n')
        result = self.run_body()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("status=interrupted", (self.home / ".local/state/smplos/update/status").read_text())

    def test_aur_and_both_flatpak_scopes_reuse_noninteractive_authentication(self):
        result = self.run_body(FLATPAK_INSTALLED="1")
        self.assertEqual(result.returncode, 0, result.stdout)
        calls = self.calls.read_text()
        self.assertIn("--sudoloop=false --sudo sudo --sudoflags=-n", calls)
        self.assertIn("flatpak --user update -y --noninteractive", calls)
        self.assertIn("sudo -n flatpak --system update -y --noninteractive", calls)
        self.assertNotIn("sudo -v", calls)
        self.assertIn("--apps-report", calls)

    def test_failed_flatpak_update_is_incomplete_and_never_reauthenticates(self):
        result = self.run_body(FLATPAK_INSTALLED="1", FLATPAK_FAIL="1")
        self.assertNotEqual(result.returncode, 0)
        calls = self.calls.read_text()
        self.assertNotIn("--finalize", calls)
        self.assertNotIn("sudo -v", calls)
        self.assertIn("system Flatpak update failed", result.stdout)
        self.assertNotIn("Update complete!", result.stdout)

    def test_refresh_reexecutes_new_wrapper_keeps_lock_and_authenticates_once(self):
        # Exercise the real front-end/bootstrap, replacing only terminal/root
        # presentation guards and the separately tested hardware power reader.
        source = self.source.split("UPDATE_SCRIPT=$(mktemp", 1)[0]
        source = source.replace("if [[ ! -t 1 && $EUID -ne 0 ]]; then", "if false; then")
        source = source.replace("if [[ $EUID -eq 0 ]]; then", "if false; then", 1)
        source = source.replace(function(BIN / "smplos-update", "check_update_power"),
                                'check_update_power() {\n return "${POWER_FAIL:-0}"\n}')
        old = self.base / "old-update"
        fresh = self.bin / "smplos-update"
        old.write_text(source + "\necho OLD-WRAPPER\n")
        fresh.write_text(source + "\necho NEW-WRAPPER\n")
        fresh.chmod(0o755)
        # The refresh keepalive must not become a busy loop in this fixture.
        (self.bin / "sleep").unlink()
        result = subprocess.run(["bash", str(old)], capture_output=True, text=True,
                                env=self.env, timeout=10)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("NEW-WRAPPER", result.stdout)
        self.assertNotIn("OLD-WRAPPER", result.stdout)
        self.assertEqual(self.calls.read_text().splitlines().count("sudo -v"), 1)
        self.assertEqual(self.calls.read_text().count("os-update --scripts"), 1)
        self.calls.write_text("")
        result = subprocess.run(["bash", str(old)], capture_output=True, text=True,
                                env={**self.env, "POWER_FAIL": "1"}, timeout=10)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(self.calls.read_text(), "")
        self.calls.write_text("")
        result = subprocess.run(["bash", str(old)], capture_output=True, text=True,
                                env={**self.env, "BOOTSTRAP_FAIL": "1"}, timeout=10)
        self.assertNotEqual(result.returncode, 0)
        self.assertNotIn("WRAPPER", result.stdout)
        self.assertIn("Updater bootstrap failed", self.log.read_text())

        self.calls.write_text("")
        result = subprocess.run(["bash", str(old)], capture_output=True, text=True,
                                env={**self.env, "BOOTSTRAP_VERIFY_FAIL": "1"}, timeout=10)
        self.assertNotEqual(result.returncode, 0)
        self.assertNotIn("WRAPPER", result.stdout)
        self.assertNotIn("pacman ", self.calls.read_text())
        self.assertIn("No package upgrade was started", result.stdout + result.stderr)

    def test_terminal_launch_preserves_scripts_only_enrollment(self):
        self.mock("fixture-terminal", 'echo "terminal $*" >> "$CALLS"\n')
        script = self.base / "launch"
        source = self.source.split("UPDATE_SCRIPT=$(mktemp", 1)[0]
        source = source.replace("if [[ ! -t 1 && $EUID -ne 0 ]]; then", "if true; then")
        script.write_text(source)
        result = subprocess.run(["bash", str(script), "--bootstrap-only"], capture_output=True,
                                text=True, env={**self.env, "SMPLOS_TERMINAL": str(self.bin / "fixture-terminal")})
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("--bootstrap-only", self.calls.read_text())
        self.assertNotIn("sudo ", self.calls.read_text())


class BootstrapEnrollmentTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.base = Path(self.temp.name)
        self.home = self.base / "home"
        self.system = self.base / "system"
        self.bin = self.system / "usr/local/bin"
        self.bin.mkdir(parents=True)
        self.upstream = self.base / "upstream"
        self.repo = self.home / ".local/share/smplos/repo"
        self.calls = self.base / "calls"
        self.git = shutil.which("git")
        self.env = {
            **os.environ, "HOME": str(self.home), "PATH": f"{self.bin}:/usr/bin:/bin",
            "GIT_CONFIG_COUNT": "1",
            "GIT_CONFIG_KEY_0": "url." + str(self.upstream) + ".insteadOf",
            "GIT_CONFIG_VALUE_0": "https://github.com/smpl-os/smplos.git",
            "CALLS": str(self.calls), "FIXTURE_ROOT": str(self.base),
        }
        self.command("init", "-q", "--initial-branch=main", str(self.upstream))
        self.command("-C", str(self.upstream), "config", "user.name", "KonTy")
        self.command("-C", str(self.upstream), "config", "user.email", "fixture@example.invalid")
        # These are the actual legacy files, not a renamed new-generation mock.
        for name in ("smplos-update", "smplos-os-update"):
            legacy = subprocess.check_output(
                [self.git, "show", f"e4188ff:src/shared/bin/{name}"], cwd=ROOT, text=True)
            target = self.upstream / "src/shared/bin" / name
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text(legacy)
            (self.bin / name).write_text(legacy)
            (self.bin / name).chmod(0o755)
        self.command("-C", str(self.upstream), "add", ".")
        self.command("-C", str(self.upstream), "commit", "-qm", "Fixture legacy installation")
        self.repo.parent.mkdir(parents=True)
        self.command("clone", "-q", "https://github.com/smpl-os/smplos.git", str(self.repo))
        for name in ("smplos-update", "smplos-os-update", "smplos-update-check",
                     "smplos-update-bootstrap"):
            source = (BIN / name).read_text()
            if name == "smplos-update":
                source = source.replace("if [[ ! -t 1 && $EUID -ne 0 ]]; then", "if false; then")
                source = source.replace("if [[ $EUID -eq 0 ]]; then", "if false; then", 1)
                source = source.replace(function(BIN / name, "check_update_power"),
                                        'check_update_power() {\n return "${POWER_FAIL:-0}"\n}')
            elif name == "smplos-update-bootstrap":
                source = source.replace('[[ $EUID -ne 0 ]]', 'true')
            elif name == "smplos-update-check":
                source = source.replace('root=Path("/")', f'root=Path("{self.system}")')
            source = source.replace("/usr/local", str(self.system / "usr/local"))
            target = self.upstream / "src/shared/bin" / name
            target.write_text(source)
        (self.upstream / "src/shared/bin/rebuild-app-cache").write_text(
            '#!/bin/bash\necho cache >> "$CALLS"\n')
        library = self.upstream / "src/shared/lib"
        library.mkdir()
        (library / "fixture.sh").write_text("fixture_helper() { :; }\n")
        self.command("-C", str(self.upstream), "add", ".")
        self.command("-C", str(self.upstream), "commit", "-qm", "Fixture new bootstrap")
        self.bootstrap = self.base / "enroll"
        self.bootstrap.write_text((self.upstream / "src/shared/bin/smplos-update-bootstrap").read_text())
        self.mock("sudo", """#!/usr/bin/python3
import os, subprocess, sys
args = sys.argv[1:]
with open(os.environ["CALLS"], "a") as stream:
    stream.write("sudo " + " ".join(args) + "\\n")
if args == ["-v"]:
    sys.exit(0)
assert args.pop(0) == "-n", args
assert args[0] in ("mkdir", "mktemp", "install", "mv", "rm"), args
assert all(not arg.startswith("/") or arg.startswith(os.environ["FIXTURE_ROOT"] + "/")
           for arg in args), args
sys.exit(subprocess.run(args).returncode)
""")
        for name in ("pacman", "yay", "smplos-ensure-yay", "pkexec", "smplos-update-apps", "hyprctl"):
            self.mock(name, '#!/bin/bash\necho FORBIDDEN >> "$CALLS"\nexit 99\n')
        # Git expands insteadOf in get-url too; expose the configured origin
        # while keeping all actual fetch traffic inside the local fixture.
        self.mock("git", f'''#!/bin/bash
echo "git $*" >> "$CALLS"
if [[ "${{3:-}} ${{4:-}} ${{5:-}}" == "remote get-url origin" ]]; then
    exec "{self.git}" -C "$2" config --get remote.origin.url
fi
exec "{self.git}" "$@"
''')

    def command(self, *args):
        return subprocess.run([self.git, *args], env=self.env, check=True,
                              capture_output=True, text=True).stdout

    def mock(self, name, text):
        path = self.bin / name
        path.write_text(text)
        path.chmod(0o755)

    def enroll(self, **env):
        return subprocess.run(["bash", str(self.bootstrap)], env={**self.env, **env},
                              capture_output=True, text=True, timeout=20)

    def test_real_legacy_install_enrolls_without_running_legacy_or_package_operations(self):
        result = self.enroll()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        calls = self.calls.read_text()
        self.assertNotIn("FORBIDDEN", calls)
        self.assertNotIn("reset", calls)
        self.assertNotIn("pull --", calls)
        self.assertEqual(calls.splitlines().count("sudo -v"), 1)
        self.assertIn("Updater enrollment verified", result.stdout)
        for source in (self.repo / "src/shared/bin").iterdir():
            self.assertEqual((self.bin / source.name).read_bytes(), source.read_bytes())
        reports = list((self.home / ".local/state/smplos/update").glob("*.bootstrap.json"))
        self.assertTrue(reports)
        report = json.loads(reports[-1].read_text())
        self.assertEqual(report["errors"], [])
        self.assertEqual(report["source_commit"], self.command("-C", str(self.repo), "rev-parse", "HEAD").strip())

    def test_dirty_checkout_stops_before_authentication_and_preserves_changes(self):
        path = self.repo / "src/shared/bin/smplos-update"
        path.write_text(path.read_text() + "\n# local work\n")
        result = self.enroll()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Local changes", result.stdout)
        self.assertIn("# local work", path.read_text())
        self.assertNotIn("sudo ", self.calls.read_text())

    def test_diverged_checkout_preserves_local_commit_and_never_authenticates(self):
        self.command("-C", str(self.repo), "config", "user.name", "KonTy")
        self.command("-C", str(self.repo), "config", "user.email", "fixture@example.invalid")
        (self.repo / "local-work").write_text("irreplaceable fixture work\n")
        self.command("-C", str(self.repo), "add", ".")
        self.command("-C", str(self.repo), "commit", "-qm", "Fixture local work")
        before = self.command("-C", str(self.repo), "rev-parse", "HEAD")
        result = self.enroll()
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(self.command("-C", str(self.repo), "rev-parse", "HEAD"), before)
        self.assertEqual((self.repo / "local-work").read_text(), "irreplaceable fixture work\n")
        self.assertNotIn("sudo ", self.calls.read_text())

    def test_in_progress_rebase_stops_before_fetch_or_privilege(self):
        (self.repo / ".git/rebase-merge").mkdir()
        result = self.enroll()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Unfinished Git operation", result.stdout)
        self.assertNotIn("fetch", self.calls.read_text())
        self.assertNotIn("sudo ", self.calls.read_text())

    def test_resume_flag_does_not_bypass_missing_helper_validation(self):
        self.assertEqual(self.enroll().returncode, 0)
        helper = self.system / "usr/local/lib/smplos/fixture.sh"
        helper.unlink()
        self.calls.write_text("")
        result = subprocess.run(["bash", str(self.bin / "smplos-update"), "--bootstrap-only"],
                                env={**self.env, "SMPLOS_UPDATER_REFRESHED": "1"},
                                capture_output=True, text=True, timeout=15)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("No package upgrade was started", result.stdout + result.stderr)
        self.assertNotIn("FORBIDDEN", self.calls.read_text())


class RecoveryContractsTests(unittest.TestCase):
    def test_failed_nvidia_initramfs_rebuild_remains_pending_and_retries_with_unchanged_config(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            config = directory / "nvidia.conf"
            config.write_text("old options\n")
            source = (ROOT / "migrations/20260701-011500-nvidia-suspend-resume.sh").read_text()
            source = source.replace("/etc/modprobe.d/nvidia.conf", str(config))
            source = """
sudo() { "$@"; }
lspci() { echo NVIDIA; }
systemctl() { return 0; }
mkinitcpio() { echo rebuild >> "$HOME/rebuilds"; return "$REBUILD_FAIL"; }
""" + source
            for fail in ("1", "0"):
                result = subprocess.run(["bash", "-c", source], capture_output=True, text=True,
                                        env={**os.environ, "HOME": str(directory), "REBUILD_FAIL": fail})
                self.assertEqual(result.returncode, int(fail), result.stdout + result.stderr)
                self.assertEqual((directory / ".local/state/smplos/nvidia-initramfs-pending").exists(), fail == "1")
            self.assertEqual((directory / "rebuilds").read_text().splitlines(), ["rebuild", "rebuild"])
            self.assertEqual(len(list(directory.glob("nvidia.conf.smplos-before-*"))), 1)
            self.assertEqual(next(directory.glob("nvidia.conf.smplos-before-*")).read_text(), "old options\n")

    def test_runtime_linkage_is_checked_before_replacing_a_working_binary(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            candidate, installed = directory / "candidate", directory / "installed"
            candidate.write_bytes(b"\x7fELFnew")
            installed.write_bytes(b"\x7fELFold")
            script = f'source "{ROOT}/src/shared/lib/smplos-app-bundle.sh"\n'
            script += """
ldd() { echo 'libhyprutils.so.99 => not found'; return 1; }
sudo() { echo SHOULD-NOT-INSTALL; return 99; }
smplos_install_app_binary "$1" "$2"
"""
            result = subprocess.run(["bash", "-c", script, "fixture", str(candidate), str(installed)],
                                    capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("incompatible runtime", result.stderr)
            self.assertNotIn("SHOULD-NOT-INSTALL", result.stdout)
            self.assertEqual(installed.read_bytes(), b"\x7fELFold")

    def test_os_version_publication_is_atomic_and_preserves_the_symlink(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            repo = directory / "repo"
            target = directory / "system/os-release"
            target.parent.mkdir()
            target.write_text('VERSION_ID="0.8.114"\n')
            link = directory / "os-release"
            link.symlink_to(target)
            (repo / "src/shared/system").mkdir(parents=True)
            (repo / "src/VERSION").write_text("0.8.115\n")
            (repo / "src/shared/system/os-release").write_text('VERSION_ID="@BUILD_VERSION@"\n')
            script = function(BIN / "smplos-os-update", "install_system_file")
            script += "\n" + function(BIN / "smplos-os-update", "sync_version").replace("/etc/os-release", str(link))
            script += """
ok() { :; }; warn() { echo "$*" >&2; }
sudo() {
    [[ "$1" != install || "${FAIL_INSTALL:-0}" == 0 ]] || return 1
    "$@"
}
sync_version
"""
            for fail, expected in (("1", "0.8.114"), ("0", "0.8.115")):
                result = subprocess.run(["bash", "-c", script], capture_output=True, text=True,
                                        env={**os.environ, "SMPLOS_REPO": str(repo), "FAIL_INSTALL": fail})
                self.assertEqual(result.returncode, int(fail), result.stderr)
                self.assertTrue(link.is_symlink())
                self.assertIn(expected, target.read_text())
                self.assertEqual(list(target.parent.iterdir()), [target])

    def test_bar_ensure_recovers_missing_bar_and_rejects_a_successful_but_ineffective_open(self):
        script = function(BIN / "bar-ctl", "eww_ensure")
        script += """
BAR_NAME=bar
LOG=fixture.log
visible="${VISIBLE:-0}"
eww_cmd() { [[ "$visible" != 1 ]] || echo 'bar: bar'; }
eww_reload() { echo reloaded; }
eww_start() { echo started; visible="${OPEN_EFFECTIVE:-1}"; }
eww_ensure
"""
        for visible, effective, status, action in (("1", "1", 0, "reloaded"),
                                                  ("0", "1", 0, "started"),
                                                  ("0", "0", 1, "started")):
            result = subprocess.run(["bash", "-c", script], capture_output=True, text=True,
                                    env={**os.environ, "VISIBLE": visible, "OPEN_EFFECTIVE": effective})
            self.assertEqual(result.returncode, status, result.stderr)
            self.assertIn(action, result.stdout)

    def test_failed_install_retains_errexit_and_does_not_reboot(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            installer = directory / "install.sh"
            installer.write_text("set -e\nfalse\necho SHOULD-NOT-RUN\n")
            source = function(ROOT / "src/shared/installer/automated_script.sh", "install_smplos")
            script = source + f"""
SMPLOS_USER=fixture
chroot_bash() {{
    [[ "$*" != *pacman* ]] || return 0
    bash {shlex.quote(str(installer))}
}}
reboot() {{ echo SHOULD-NOT-REBOOT; }}
install_smplos
"""
            result = subprocess.run(["bash", "-c", script], capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertNotIn("SHOULD-NOT", result.stdout)
            self.assertIn("setup failed", result.stderr)


if __name__ == "__main__":
    unittest.main()
