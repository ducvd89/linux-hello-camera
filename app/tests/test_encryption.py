"""The "lock face data to the security chip" switch: hardware detection and the switch's state."""

import contextlib
import io
import json
import os
import tempfile
import unittest
from unittest import mock

from support import load_script

import linux_hello_camera_common as common

helper = load_script("settings-helper")

SB_VAR = common.SECURE_BOOT_VAR


def make_root(tmp, tpm_major=None, tpmrm=True, secure_boot=None):
    """A fake / with the files the detection looks at. tpm_major: "2", "1.2" or None."""
    def put(path, data, mode="w"):
        full = os.path.join(tmp, path.lstrip("/"))
        os.makedirs(os.path.dirname(full), exist_ok=True)
        with open(full, mode) as f:
            f.write(data)
    if tpm_major is not None:
        put("/sys/class/tpm/tpm0/tpm_version_major", tpm_major + "\n")
    if tpmrm:
        put("/dev/tpmrm0", "")
    if secure_boot is not None:
        put(SB_VAR, bytes([7, 0, 0, 0, secure_boot]), "wb")
    return tmp


class Detection(unittest.TestCase):
    def detect(self, **kw):
        with tempfile.TemporaryDirectory() as tmp:
            return common.detect_hardware(make_root(tmp, **kw))

    def test_tpm2_with_secure_boot(self):
        self.assertEqual(self.detect(tpm_major="2", secure_boot=1), (True, True))

    def test_tpm2_without_secure_boot(self):
        self.assertEqual(self.detect(tpm_major="2", secure_boot=0), (True, False))

    def test_secure_boot_variable_missing_means_off(self):
        self.assertEqual(self.detect(tpm_major="2"), (True, False))

    def test_no_tpm(self):
        self.assertEqual(self.detect(tpmrm=False, secure_boot=1), (False, True))

    def test_tpm_1_2_is_not_enough(self):
        self.assertEqual(self.detect(tpm_major="1", secure_boot=1), (False, True))

    def test_needs_the_resource_manager_device(self):
        self.assertEqual(self.detect(tpm_major="2", tpmrm=False, secure_boot=1), (False, True))

    def test_short_or_odd_secure_boot_variable_is_off(self):
        with tempfile.TemporaryDirectory() as tmp:
            make_root(tmp, tpm_major="2")
            path = os.path.join(tmp, SB_VAR.lstrip("/"))
            os.makedirs(os.path.dirname(path))
            for data in (b"", b"\x07\x00\x00\x00", b"\x07\x00\x00\x00\x02"):
                with open(path, "wb") as f:
                    f.write(data)
                self.assertEqual(common.detect_hardware(tmp)[1], False, data)


class Effective(unittest.TestCase):
    def test_every_combination(self):
        table = {
            # (setting, tpm2, secure_boot): effective
            ("auto", True, True): "tpm-sb", ("auto", True, False): "none",
            ("auto", False, True): "none", ("auto", False, False): "none",
            ("on", True, True): "tpm-sb", ("on", True, False): "tpm",
            ("on", False, True): "none", ("on", False, False): "none",
            ("off", True, True): "none", ("off", True, False): "none",
            ("off", False, True): "none", ("off", False, False): "none",
        }
        for (setting, tpm2, sb), expected in table.items():
            self.assertEqual(common.effective_encryption(setting, tpm2, sb), expected, (setting, tpm2, sb))

    def test_local_status_reads_the_config_and_the_hardware(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = make_root(tmp, tpm_major="2", secure_boot=1)
            cfg = common.merge(common.DEFAULTS, {})
            self.assertEqual(common.local_status(cfg, root),
                             {"tpm2": True, "secure_boot": True, "setting": "auto", "effective": "tpm-sb"})
            cfg = common.merge(common.DEFAULTS, {"storage": {"tpm_encryption": "off"}})
            self.assertEqual(common.local_status(cfg, root)["effective"], "none")
            cfg = common.merge(common.DEFAULTS, {"storage": {"tpm_encryption": "banana"}})
            self.assertEqual(common.local_status(cfg, root)["setting"], "auto")

    def test_the_default_setting_is_auto(self):
        self.assertEqual(common.get(common.DEFAULTS, "storage.tpm_encryption"), "auto")


def status(tpm2, sb, setting="auto"):
    return {"tpm2": tpm2, "secure_boot": sb, "setting": setting,
            "effective": common.effective_encryption(setting, tpm2, sb)}


class SwitchState(unittest.TestCase):
    def test_tpm_and_secure_boot_default_is_on(self):
        look = common.encryption_switch(status(True, True))
        self.assertEqual((look["sensitive"], look["active"]), (True, True))

    def test_tpm_and_secure_boot_subtitle_mentions_the_binding_and_its_cost(self):
        look = common.encryption_switch(status(True, True))
        self.assertEqual(look["subtitle"], "Encrypted with the TPM and tied to Secure Boot. "
                                           "Changing Secure Boot keys means adding your face again.")

    def test_tpm_and_secure_boot_switched_off(self):
        look = common.encryption_switch(status(True, True, "off"))
        self.assertEqual((look["sensitive"], look["active"]), (True, False))

    def test_tpm_without_secure_boot_default_is_off_but_available(self):
        look = common.encryption_switch(status(True, False))
        self.assertEqual((look["sensitive"], look["active"]), (True, False))
        self.assertIn("Secure Boot is off", look["subtitle"])

    def test_tpm_without_secure_boot_switched_on(self):
        look = common.encryption_switch(status(True, False, "on"))
        self.assertEqual((look["sensitive"], look["active"]), (True, True))
        self.assertIn("Secure Boot is off", look["subtitle"])
        self.assertIn("TPM only", look["subtitle"])

    def test_no_tpm_is_greyed_out_and_off(self):
        for setting in ("auto", "on", "off"):
            for sb in (True, False):
                look = common.encryption_switch(status(False, sb, setting))
                self.assertEqual((look["sensitive"], look["active"]), (False, False), (setting, sb))
                self.assertIn("no TPM 2.0", look["subtitle"])
                self.assertIn("never photos", look["subtitle"])

    def test_every_state_has_a_subtitle(self):
        for tpm2 in (True, False):
            for sb in (True, False):
                for setting in common.TPM_SETTINGS:
                    self.assertTrue(common.encryption_switch(status(tpm2, sb, setting))["subtitle"])


class Status(unittest.TestCase):
    GOOD = {"tpm2": True, "secure_boot": False, "setting": "on", "effective": "tpm"}

    def test_parse(self):
        self.assertEqual(common.parse_status(json.dumps(self.GOOD) + "\n"), self.GOOD)

    def test_extra_fields_are_dropped(self):
        self.assertEqual(common.parse_status(json.dumps({**self.GOOD, "x": 1})), self.GOOD)

    def test_unusable_answers_fall_back(self):
        sentinel = object()
        for text in (None, "", "garbage", "{broken", "[]", "{}", json.dumps({**self.GOOD, "setting": "maybe"}),
                     json.dumps({**self.GOOD, "tpm2": "yes"}), json.dumps({**self.GOOD, "effective": "host"})):
            self.assertIs(common.parse_status(text, sentinel), sentinel, text)

    def test_the_list_answer_uses_the_three_modes(self):
        for value in ("tpm-sb", "tpm", "none"):
            out = json.dumps({"result": "ok", "entries": [], "encryption": value})
            self.assertEqual(common.parse_face_list(out)["encryption"], value)
        for value in ("host", "host+tpm2", "tpm2", None):
            out = json.dumps({"result": "ok", "entries": [], "encryption": value})
            self.assertIsNone(common.parse_face_list(out)["encryption"])


class SetEncryption(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.tmp = tmp.name
        path = os.path.join(self.tmp, "engine")
        with open(path, "w") as f:
            f.write('#!/bin/sh\necho "{\\"result\\": \\"ok\\", \\"args\\": \\"$*\\"}"\n')
        os.chmod(path, 0o755)
        for p in (mock.patch.object(helper, "ENGINE", path), mock.patch.object(helper.os, "geteuid", return_value=0)):
            p.start()
            self.addCleanup(p.stop)

    def run_cmd(self, *argv, env=None):
        out = io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(io.StringIO()):
            with mock.patch.dict(os.environ, env or {}, clear=True):
                code = helper.main(list(argv))
        return code, out.getvalue()

    def test_valid_values_are_forwarded(self):
        for value in ("on", "off", "auto"):
            code, out = self.run_cmd("set-encryption", value)
            self.assertEqual(code, 0)
            self.assertEqual(json.loads(out)["args"], "set-encryption " + value)

    def test_no_user_is_needed(self):
        # The package scripts and installer run system-wide commands as plain root
        self.assertEqual(self.run_cmd("set-encryption", "off", env={})[0], 0)

    def test_everything_else_is_refused_and_the_engine_never_runs(self):
        for value in ("", "ON", "true", "yes", "1", "tpm2", "on off", "--help", "on;ls", "../on"):
            with self.assertRaises(SystemExit, msg=repr(value)):
                self.run_cmd("set-encryption", value)

    def test_needs_exactly_one_value(self):
        with self.assertRaises(SystemExit):
            self.run_cmd("set-encryption")
        with self.assertRaises(SystemExit):
            self.run_cmd("set-encryption", "on", "off")

    def test_the_engine_failure_is_passed_on(self):
        path = os.path.join(self.tmp, "engine")
        with open(path, "w") as f:
            f.write('#!/bin/sh\necho \'{"result": "error", "detail": "no TPM 2.0"}\'\nexit 1\n')
        code, out = self.run_cmd("set-encryption", "on")
        self.assertEqual(code, 1)
        self.assertEqual(json.loads(out)["detail"], "no TPM 2.0")


class Schema(unittest.TestCase):
    def test_plain_set_accepts_only_the_three_values(self):
        for value in ("auto", "on", "off"):
            self.assertEqual(helper.validate("storage.tpm_encryption", value), value)
        for value in ("true", "yes", "", "host", "tpm2"):
            with self.assertRaises(SystemExit), contextlib.redirect_stderr(io.StringIO()):
                helper.validate("storage.tpm_encryption", value)


if __name__ == "__main__":
    unittest.main()
