"""confirm-broker `start`: no early dialog for users without enrolled face templates."""

import os
import tempfile
import unittest
from unittest import mock

from support import load_script

import linux_hello_camera_common as common

broker = load_script("confirm-broker")

ENV = {"PAM_SERVICE": "sudo", "PAM_TYPE": "auth", "PAM_USER": "someone"}


class Start(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.templates = tmp.name
        patches = [
            mock.patch.object(common, "TEMPLATES_DIR", self.templates),
            mock.patch.object(common, "confirm_enabled", return_value=True),
            mock.patch.object(common, "confirm_timeout", return_value=15),
            mock.patch.object(broker.os, "geteuid", return_value=0),
            mock.patch.object(broker.pwd, "getpwnam", return_value=mock.Mock(pw_name="someone")),
            mock.patch.dict(os.environ, ENV),
        ]
        for p in patches:
            p.start()
            self.addCleanup(p.stop)
        # Stops start() right after the faces check if it gets that far
        self.desktop = mock.patch.object(common, "desktop_env", return_value=None)
        self.desktop_mock = self.desktop.start()
        self.addCleanup(self.desktop.stop)

    def enrol(self, user="someone", ext=".cred"):
        with open(os.path.join(self.templates, user + ext), "wb") as f:
            f.write(b"encrypted")

    def test_enrolled_user_gets_the_dialog(self):
        self.enrol()
        self.assertEqual(broker.start(), 0)
        self.desktop_mock.assert_called_once()

    def test_every_kind_of_template_counts(self):
        for ext in (".json", ".tpm.cred", ".tpm-sb.cred"):
            self.desktop_mock.reset_mock()
            for name in os.listdir(self.templates):
                os.remove(os.path.join(self.templates, name))
            self.enrol(ext=ext)
            broker.start()
            self.desktop_mock.assert_called_once()

    def test_user_without_a_template_gets_nothing(self):
        self.assertEqual(broker.start(), 0)
        self.desktop_mock.assert_not_called()

    def test_other_files_do_not_count(self):
        open(os.path.join(self.templates, "someone.txt"), "w").close()
        open(os.path.join(self.templates, "someone.cred.tmp"), "w").close()
        broker.start()
        self.desktop_mock.assert_not_called()

    def test_another_users_template_does_not_count(self):
        self.enrol("other")
        broker.start()
        self.desktop_mock.assert_not_called()


if __name__ == "__main__":
    unittest.main()
