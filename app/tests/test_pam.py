"""PAM editing in settings-helper, on temporary copies of realistic files."""

import contextlib
import io
import os
import tempfile
import unittest
from unittest import mock

from support import load_script

helper = load_script("settings-helper")

SUDO = """#%PAM-1.0
auth		include		system-auth
account		include		system-auth
session		include		system-auth
"""

# Arch's vendor files (only in /usr/lib/pam.d until something overrides them)
POLKIT = """#%PAM-1.0

auth       include      system-auth
account    include      system-auth
password   include      system-auth
session    include      system-auth
"""

KDE_FINGERPRINT = """#%PAM-1.0

auth     required       pam_shells.so
auth     requisite      pam_nologin.so
auth     required       pam_faillock.so preauth
-auth    sufficient     pam_fprintd.so
auth     required       pam_deny.so
account  include        system-auth
"""

KDE = """#%PAM-1.0
auth       include      system-auth
account    include      system-auth
password   include      system-auth
session    include      system-auth
"""

GDM = """auth     [success=ok default=1] pam_gdm.so
auth     optional                    pam_gnome_keyring.so
auth     include                     system-local-login
account  include                     system-local-login
"""

MODULE_LINE = "auth       sufficient   pam_linux_hello_camera.so   # linux-hello-camera\n"

MARK = helper.PAM_MARK


def run(*argv):
    out = io.StringIO()
    with contextlib.redirect_stdout(out):
        code = helper.main(list(argv))
    return code, out.getvalue()


class PamCase(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.etc = os.path.join(tmp.name, "etc-pam.d")
        self.vendor = os.path.join(tmp.name, "usr-lib-pam.d")
        os.makedirs(self.etc)
        os.makedirs(self.vendor)
        self.module = os.path.join(tmp.name, "pam_linux_hello_camera.so")
        open(self.module, "w").close()
        for name, value in (("PAM_DIR", self.etc), ("VENDOR_PAM_DIR", self.vendor),
                            ("PAM_MODULE_GLOB", self.module)):
            patcher = mock.patch.object(helper, name, value)
            patcher.start()
            self.addCleanup(patcher.stop)
        geteuid = mock.patch.object(helper.os, "geteuid", return_value=0)
        geteuid.start()
        self.addCleanup(geteuid.stop)

    def write(self, service, text, vendor=False):
        path = os.path.join(self.vendor if vendor else self.etc, service)
        with open(path, "w") as f:
            f.write(text)
        return path

    def read(self, service):
        with open(os.path.join(self.etc, service)) as f:
            return f.read()

    def exists(self, service):
        return os.path.exists(os.path.join(self.etc, service))

    def lines(self, service):
        return self.read(service).splitlines()

    def code_lines(self, service):
        return [l for l in self.lines(service) if not l.startswith("#")]


class EnableDisable(PamCase):
    def test_sudo_gets_broker_lines_around_the_module(self):
        self.write("sudo", SUDO)
        run("pam-enable", "sudo")
        lines = self.code_lines("sudo")
        self.assertIn("confirm-broker start", lines[0])
        self.assertIn("pam_linux_hello_camera.so", lines[1])
        self.assertIn("confirm-broker failed", lines[2])
        self.assertIn("system-auth", lines[3])
        self.assertTrue(all(MARK in l for l in lines[:3]))
        # The dialog helpers can only ever be "optional", the face check "sufficient"
        self.assertRegex(lines[0], r"^auth\s+optional\s")
        self.assertRegex(lines[1], r"^auth\s+sufficient\s")
        self.assertRegex(lines[2], r"^auth\s+optional\s")
        self.assertIn("/usr/lib/linux-hello-camera/confirm-broker", lines[0])

    def test_disable_restores_the_original_exactly(self):
        self.write("sudo", SUDO)
        run("pam-enable", "sudo")
        run("pam-disable", "sudo")
        self.assertEqual(self.read("sudo"), SUDO)

    def test_idempotent(self):
        self.write("sudo", SUDO)
        run("pam-enable", "sudo")
        once = self.read("sudo")
        run("pam-enable", "sudo")
        self.assertEqual(self.read("sudo"), once)
        self.assertEqual(once.count("pam_linux_hello_camera.so"), 1)
        run("pam-disable", "sudo")
        run("pam-disable", "sudo")
        self.assertEqual(self.read("sudo"), SUDO)

    def test_login_services_get_only_the_module_line(self):
        self.write("gdm-password", GDM)
        run("pam-enable", "gdm-password")
        lines = self.code_lines("gdm-password")
        self.assertRegex(lines[0], r"^auth\s+sufficient\s+pam_linux_hello_camera\.so\s+# linux-hello-camera$")
        self.assertEqual(sum(MARK in l for l in lines), 1)
        self.assertEqual(lines[1:], GDM.splitlines())

    def test_goes_before_fingerprint_not_before_preauth_checks(self):
        self.write("kde-fingerprint", KDE_FINGERPRINT)
        self.write("kde", KDE)
        run("pam-enable", "kde-fingerprint")
        lines = self.code_lines("kde-fingerprint")
        gate = next(i for i, l in enumerate(lines) if "input-gate" in l)
        module = next(i for i, l in enumerate(lines) if "pam_linux_hello_camera.so" in l)
        fprint = next(i for i, l in enumerate(lines) if "pam_fprintd.so" in l)
        faillock = next(i for i, l in enumerate(lines) if "pam_faillock.so" in l)
        self.assertLess(faillock, gate)
        self.assertEqual(module, gate + 1)
        self.assertEqual(fprint, module + 1)
        # The gate must refuse (requisite), so the face check can't run without a key press
        self.assertRegex(lines[gate], r"^auth\s+requisite\s+pam_exec\.so quiet quiet_log /usr/lib/linux-hello-camera/input-gate\s")

    def test_lock_screen_release_line_is_last_auth_line_in_kde(self):
        self.write("kde-fingerprint", KDE_FINGERPRINT)
        self.write("kde", KDE)
        run("pam-enable", "kde-fingerprint")
        auth = [l for l in self.lines("kde") if l.startswith("auth")]
        self.assertIn("system-auth", auth[0])
        self.assertIn("input-gate --release", auth[-1])
        self.assertRegex(auth[-1], r"^auth\s+optional\s")
        self.assertEqual(sum("input-gate" in l for l in self.lines("kde")), 1)

    def test_lock_screen_disable_cleans_both_files(self):
        self.write("kde-fingerprint", KDE_FINGERPRINT, vendor=True)
        self.write("kde", KDE)
        run("pam-enable", "kde-fingerprint")
        self.assertTrue(self.exists("kde-fingerprint"))
        run("pam-disable", "kde-fingerprint")
        self.assertFalse(self.exists("kde-fingerprint"))
        self.assertEqual(self.read("kde"), KDE)

    def test_enable_moves_a_face_check_out_of_kde(self):
        self.write("kde-fingerprint", KDE_FINGERPRINT)
        self.write("kde", KDE.replace("auth       include", MODULE_LINE + "auth       include", 1))
        run("pam-enable", "kde-fingerprint")
        self.assertNotIn("pam_linux_hello_camera.so", self.read("kde"))

    def test_unknown_service_is_rejected(self):
        for name in ("login", "../shadow", "sshd", ""):
            with self.assertRaises(SystemExit):
                with contextlib.redirect_stderr(io.StringIO()):
                    run("pam-enable", name)
        with self.assertRaises(SystemExit):
            with contextlib.redirect_stderr(io.StringIO()):
                run("pam-disable", "sudo", "login")
        self.assertEqual(os.listdir(self.etc), [])

    def test_a_bad_service_stops_the_whole_command(self):
        self.write("sudo", SUDO)
        with self.assertRaises(SystemExit):
            with contextlib.redirect_stderr(io.StringIO()):
                run("pam-enable", "sudo", "login")
        self.assertEqual(self.read("sudo"), SUDO)

    def test_refuses_when_the_module_is_not_installed(self):
        os.remove(self.module)
        self.write("sudo", SUDO)
        with self.assertRaises(SystemExit):
            with contextlib.redirect_stderr(io.StringIO()):
                run("pam-enable", "sudo")
        self.assertEqual(self.read("sudo"), SUDO)

    def test_must_run_as_root(self):
        with mock.patch.object(helper.os, "geteuid", return_value=1000):
            with self.assertRaises(SystemExit):
                with contextlib.redirect_stderr(io.StringIO()):
                    run("pam-enable", "sudo")


class VendorCopiesAndBackups(PamCase):
    def test_vendor_only_file_is_copied_with_a_marker_and_deleted_again(self):
        self.write("polkit-1", POLKIT, vendor=True)
        run("pam-enable", "polkit-1")
        text = self.read("polkit-1")
        self.assertIn("# linux-hello-camera: copied from " + os.path.join(self.vendor, "polkit-1"), text)
        self.assertEqual(text.splitlines()[0], "#%PAM-1.0")
        self.assertIn("pam_linux_hello_camera.so", text)
        run("pam-disable", "polkit-1")
        self.assertFalse(self.exists("polkit-1"))
        self.assertFalse(os.path.exists(os.path.join(self.etc, "polkit-1.linux-hello-camera.bak")))

    def test_vendor_copy_is_kept_when_it_differs_from_the_vendor_file(self):
        self.write("polkit-1", POLKIT, vendor=True)
        run("pam-enable", "polkit-1")
        with open(os.path.join(self.etc, "polkit-1"), "a") as f:
            f.write("session    optional     pam_echo.so\n")
        run("pam-disable", "polkit-1")
        self.assertTrue(self.exists("polkit-1"))
        self.assertIn("pam_echo.so", self.read("polkit-1"))
        self.assertNotIn("pam_linux_hello_camera.so", self.read("polkit-1"))

    def test_real_file_is_backed_up_once(self):
        self.write("sudo", SUDO)
        bak = os.path.join(self.etc, "sudo.linux-hello-camera.bak")
        run("pam-enable", "sudo")
        with open(bak) as f:
            self.assertEqual(f.read(), SUDO)
        # A later change by the admin must not overwrite the first backup
        with open(os.path.join(self.etc, "sudo"), "a") as f:
            f.write("# changed\n")
        run("pam-disable", "sudo")
        run("pam-enable", "sudo")
        with open(bak) as f:
            self.assertEqual(f.read(), SUDO)

    def test_vendor_copy_is_not_backed_up(self):
        self.write("polkit-1", POLKIT, vendor=True)
        run("pam-enable", "polkit-1")
        self.assertEqual(sorted(os.listdir(self.etc)), ["polkit-1"])
        run("pam-enable", "polkit-1")
        self.assertEqual(sorted(os.listdir(self.etc)), ["polkit-1"])

    def test_missing_everywhere_is_an_error(self):
        with self.assertRaises(SystemExit):
            with contextlib.redirect_stderr(io.StringIO()):
                run("pam-enable", "sddm")

    def test_files_are_written_atomically_with_mode_644(self):
        self.write("sudo", SUDO)
        os.chmod(os.path.join(self.etc, "sudo"), 0o600)
        run("pam-enable", "sudo")
        self.assertEqual(os.stat(os.path.join(self.etc, "sudo")).st_mode & 0o777, 0o644)
        self.assertFalse([n for n in os.listdir(self.etc) if n.startswith(".")])


class Refresh(PamCase):
    """pam-refresh (run on upgrades) rewrites what is on with the current lines."""

    def enable_all(self):
        self.write("sudo", SUDO)
        self.write("gdm-password", GDM)
        self.write("kde-fingerprint", KDE_FINGERPRINT)
        self.write("kde", KDE)
        run("pam-enable", "sudo", "gdm-password", "kde-fingerprint")

    def test_refresh_keeps_a_working_setup_as_it_is(self):
        self.enable_all()
        before = {s: self.read(s) for s in ("sudo", "gdm-password", "kde-fingerprint", "kde")}
        run("pam-refresh")
        self.assertEqual({s: self.read(s) for s in before}, before)

    def test_refresh_repairs_a_setup_that_lost_its_broker_lines(self):
        self.write("sudo", SUDO)
        run("pam-enable", "sudo")
        text = "".join(l for l in self.read("sudo").splitlines(True) if "confirm-broker" not in l)
        self.write("sudo", text)
        run("pam-refresh")
        self.assertEqual(self.read("sudo").count("confirm-broker"), 2)

    def test_refresh_repairs_a_lock_screen_that_lost_its_gate(self):
        self.enable_all()
        text = "".join(l for l in self.read("kde-fingerprint").splitlines(True) if "input-gate" not in l)
        self.write("kde-fingerprint", text)
        run("pam-refresh")
        self.assertEqual(self.read("kde-fingerprint").count("input-gate"), 1)
        self.assertEqual(self.read("kde").count("input-gate --release"), 1)

    def test_refresh_brings_old_lines_up_to_date(self):
        self.write("sudo", SUDO)
        old_start = ("auth       optional     pam_exec.so seteuid quiet quiet_log "
                     "/usr/lib/linux-hello-camera/confirm-broker start   " + MARK + "\n")
        self.write("sudo", "#%PAM-1.0\n" + old_start + MODULE_LINE + SUDO.split("\n", 1)[1])
        run("pam-refresh")
        lines = self.code_lines("sudo")
        self.assertIn("confirm-broker start", lines[0])
        self.assertIn("pam_linux_hello_camera.so", lines[1])
        self.assertIn("confirm-broker failed", lines[2])
        self.assertEqual(self.read("sudo").count("pam_linux_hello_camera.so"), 1)

    def test_refresh_keeps_a_vendor_copy_removable(self):
        self.write("polkit-1", POLKIT, vendor=True)
        run("pam-enable", "polkit-1")
        run("pam-refresh")
        self.assertIn("# linux-hello-camera: copied from", self.read("polkit-1"))
        run("pam-disable", "polkit-1")
        self.assertFalse(self.exists("polkit-1"))

    def test_refresh_leaves_services_without_face_unlock_alone(self):
        self.write("sudo", SUDO)
        self.write("gdm-password", GDM)
        run("pam-refresh")
        self.assertEqual(self.read("sudo"), SUDO)
        self.assertEqual(self.read("gdm-password"), GDM)
        self.assertEqual(sorted(os.listdir(self.etc)), ["gdm-password", "sudo"])

    def test_refresh_without_the_module_installed_changes_nothing(self):
        self.write("sudo", "#%PAM-1.0\n" + MODULE_LINE + SUDO.split("\n", 1)[1])
        before = self.read("sudo")
        os.remove(self.module)
        with self.assertRaises(SystemExit):
            with contextlib.redirect_stderr(io.StringIO()):
                run("pam-refresh")
        self.assertEqual(self.read("sudo"), before)


if __name__ == "__main__":
    unittest.main()
