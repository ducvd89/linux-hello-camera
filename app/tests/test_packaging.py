"""The package's scripts: syntax and the pieces the daemon needs."""

import os
import shutil
import subprocess
import unittest

from support import APP_DIR

ROOT = os.path.dirname(APP_DIR)
INSTALL = os.path.join(ROOT, "linux-hello-camera.install")


def read(path):
    with open(path) as f:
        return f.read()


@unittest.skipUnless(shutil.which("bash"), "bash is needed")
class Syntax(unittest.TestCase):
    def test_bash_accepts_the_scripts(self):
        for path in (INSTALL, os.path.join(ROOT, "install.sh")):
            result = subprocess.run(["bash", "-n", path], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)

    def test_the_install_script_defines_the_hooks(self):
        script = (f'source "{INSTALL}"; '
                  'for f in post_install post_upgrade pre_remove post_remove; do type -t $f || echo missing $f; done')
        out = subprocess.run(["bash", "-c", script], capture_output=True, text=True).stdout.split()
        self.assertEqual(out, ["function"] * 4)


class InstallScript(unittest.TestCase):
    def hook(self, name):
        text = read(INSTALL)
        start = text.index(name + "() {")
        return text[start:text.index("\n}\n", start)]

    def test_the_daemon_is_started_on_install_and_upgrade(self):
        start = self.hook("start_daemon")
        self.assertIn("systemctl daemon-reload", start)
        self.assertIn("systemctl enable --now linux-hello-camerad.socket", start)
        for name in ("post_install", "post_upgrade"):
            body = self.hook(name)
            self.assertIn("start_daemon", body)
            self.assertIn("pam-refresh", body)

    def test_old_faces_are_migrated(self):
        self.assertIn("migrate", self.hook("post_upgrade"))
        self.assertIn("/var/lib/linux-hello-camera/faces", self.hook("post_install"))
        self.assertIn("migrate", self.hook("post_install"))

    def test_removal_turns_pam_off_before_the_daemon(self):
        body = self.hook("pre_remove")
        self.assertLess(body.index("pam-disable"), body.index("systemctl disable --now linux-hello-camerad.socket linux-hello-camerad.service"))

    def test_removal_says_where_the_templates_stay(self):
        body = self.hook("post_remove")
        self.assertIn("/var/lib/linux-hello-camera/templates", body)
        self.assertIn("sudo rm -r /var/lib/linux-hello-camera", body)


class Pkgbuild(unittest.TestCase):
    def test_version_and_dependencies(self):
        out = subprocess.run(["bash", "-c", f'source "{os.path.join(ROOT, "PKGBUILD")}"; '
                              'echo "$pkgver"; echo "${depends[@]}"; echo "${backup[@]}"'],
                             capture_output=True, text=True).stdout.splitlines()
        self.assertRegex(out[0], r"^\d+\.\d+\.\d+$")
        # The About dialog shows the same version as the package
        self.assertIn(f'VERSION = "{out[0]}"', read(os.path.join(APP_DIR, "linux_hello_camera.py")))
        self.assertIn("systemd", out[1].split())
        self.assertIn("etc/linux-hello-camera/config.yaml", out[2].split())

    def test_the_polkit_drop_in_no_longer_hands_out_devices(self):
        text = read(os.path.join(APP_DIR, "10-linux-hello-camera.conf"))
        code = [l for l in text.splitlines() if l.strip() and not l.startswith("#")]
        self.assertEqual(code, ["[Service]", "ReadWritePaths=-/run/linux-hello-camera", "ProtectHome=read-only"])


if __name__ == "__main__":
    unittest.main()
