"""config.yaml handling in settings-helper and the shared module."""

import contextlib
import io
import os
import tempfile
import unittest
from unittest import mock

from support import load_script

import linux_hello_camera_common as common

helper = load_script("settings-helper")


def run_set(*pairs):
    out = io.StringIO()
    with contextlib.redirect_stdout(out):
        code = helper.main(["set", *pairs])
    return code, out.getvalue()


def fails(*pairs):
    """Run `set` expecting it to refuse; returns the message."""
    err = io.StringIO()
    with contextlib.redirect_stderr(err), self_assert_exit():
        run_set(*pairs)
    return err.getvalue()


@contextlib.contextmanager
def self_assert_exit():
    try:
        yield
    except SystemExit as e:
        if e.code in (0, None):
            raise AssertionError("expected a refusal, got exit 0")
        return
    raise AssertionError("expected the helper to exit with an error")


class ConfigCase(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.dir = os.path.join(tmp.name, "etc", "linux-hello-camera")
        self.path = os.path.join(self.dir, "config.yaml")
        for target in (helper, common):
            patcher = mock.patch.object(target, "CONFIG_PATH", self.path)
            patcher.start()
            self.addCleanup(patcher.stop)
        geteuid = mock.patch.object(helper.os, "geteuid", return_value=0)
        geteuid.start()
        self.addCleanup(geteuid.stop)

    def write(self, text, mode=0o644):
        os.makedirs(self.dir, exist_ok=True)
        with open(self.path, "w") as f:
            f.write(text)
        os.chmod(self.path, mode)

    def saved(self):
        with open(self.path) as f:
            return common.yaml.safe_load(f)


class Defaults(ConfigCase):
    def test_missing_file_gives_the_defaults(self):
        self.assertEqual(common.load_config(), common.DEFAULTS)

    def test_defaults_match_the_design(self):
        d = common.DEFAULTS
        self.assertEqual((d["disabled"], d["camera"], d["timeout_ms"], d["abort_if_lid_closed"], d["abort_if_ssh"]),
                         (False, "", 4000, True, True))
        self.assertEqual(d["recognition"], {"model": "edgeface_s_gamma_05.onnx", "threshold": 0.5, "frames_needed": 2})
        self.assertEqual(d["detection"], {"model": "yolov8n-face.onnx", "threshold": 0.5})
        self.assertEqual(d["ir_liveness"], {"enabled": True, "min_pairs": 2, "min_face_gain": 25.0, "min_gain_ratio": 1.8})
        self.assertEqual(d["ai_antispoof"], {"enabled": False, "model": "minifas_v2.onnx", "threshold": 0.8})
        self.assertEqual(d["storage"], {"tpm_encryption": "auto"})
        self.assertEqual(d["confirm"], {"enabled": True, "services": ["sudo", "polkit-1"], "timeout_s": 15})

    def test_the_config_the_package_ships_is_the_defaults(self):
        shipped = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "config.yaml")
        self.assertEqual(common.read_raw(shipped), common.DEFAULTS)

    def test_partial_file_is_filled_in(self):
        self.write("recognition:\n  threshold: 0.7\nconfirm:\n  enabled: false\n")
        cfg = common.load_config()
        self.assertEqual(cfg["recognition"]["threshold"], 0.7)
        self.assertEqual(cfg["recognition"]["frames_needed"], 2)
        self.assertFalse(cfg["confirm"]["enabled"])
        self.assertEqual(cfg["confirm"]["timeout_s"], 15)

    def test_empty_file_gives_the_defaults(self):
        self.write("")
        self.assertEqual(common.load_config(), common.DEFAULTS)

    def test_broken_yaml_is_an_error(self):
        self.write("recognition: [unclosed\n")
        with self.assertRaises(common.ConfigError):
            common.load_config()
        self.write("- just\n- a list\n")
        with self.assertRaises(common.ConfigError):
            common.load_config()

    def test_defaults_are_not_shared_between_loads(self):
        cfg = common.load_config()
        cfg["confirm"]["services"].append("sddm")
        self.assertEqual(common.DEFAULTS["confirm"]["services"], ["sudo", "polkit-1"])

    def test_every_settable_key_exists_in_the_defaults_and_accepts_its_default(self):
        for key, rule in helper.CONFIG_SCHEMA.items():
            default = common.get(common.DEFAULTS, key)
            self.assertIsNotNone(default, key)
            if rule[0] == "device":
                continue
            text = str(default).lower() if isinstance(default, bool) else str(default)
            self.assertEqual(helper.validate(key, text), default, key)

    def test_get_with_missing_levels(self):
        self.assertEqual(common.get({"a": {"b": 1}}, "a.b"), 1)
        self.assertEqual(common.get({"a": {"b": 1}}, "a.c", 7), 7)
        self.assertEqual(common.get({"a": 1}, "a.b", 7), 7)


class SetValues(ConfigCase):
    def test_creates_the_file_from_the_defaults(self):
        code, out = run_set("recognition.threshold=0.6")
        self.assertEqual(code, 0)
        self.assertIn("recognition.threshold = 0.6", out)
        data = self.saved()
        self.assertEqual(data["recognition"]["threshold"], 0.6)
        self.assertEqual(data["recognition"]["frames_needed"], 2)
        self.assertEqual(data["timeout_ms"], 4000)
        self.assertEqual(os.stat(self.path).st_mode & 0o777, 0o644)

    def test_values_get_the_right_types(self):
        run_set("recognition.threshold=0.55", "recognition.frames_needed=3", "disabled=true",
                "timeout_ms=6000", "ir_liveness.min_face_gain=30", "confirm.enabled=false",
                "recognition.model=edgeface_xs_gamma_06.onnx")
        data = self.saved()
        self.assertIsInstance(data["recognition"]["threshold"], float)
        self.assertIsInstance(data["recognition"]["frames_needed"], int)
        self.assertIs(data["disabled"], True)
        self.assertEqual(data["timeout_ms"], 6000)
        self.assertEqual(data["ir_liveness"]["min_face_gain"], 30.0)
        self.assertIs(data["confirm"]["enabled"], False)
        self.assertEqual(data["recognition"]["model"], "edgeface_xs_gamma_06.onnx")

    def test_abort_if_ssh(self):
        run_set("abort_if_ssh=false")
        self.assertIs(self.saved()["abort_if_ssh"], False)
        run_set("abort_if_ssh=true")
        self.assertIs(self.saved()["abort_if_ssh"], True)
        self.assertIn("true or false", fails("abort_if_ssh=yes"))

    def test_min_pairs_is_limited_to_1_to_3(self):
        run_set("ir_liveness.min_pairs=1")
        run_set("ir_liveness.min_pairs=3")
        self.assertEqual(self.saved()["ir_liveness"]["min_pairs"], 3)

    def test_one_call_can_set_several_keys(self):
        code, out = run_set("disabled=false", "confirm.timeout_s=20")
        self.assertEqual(out.splitlines(), ["disabled = false", "confirm.timeout_s = 20"])

    def test_range_edges(self):
        for pair in ("recognition.threshold=0.30", "recognition.threshold=0.90", "recognition.frames_needed=1",
                     "recognition.frames_needed=3", "timeout_ms=1000", "timeout_ms=15000",
                     "confirm.timeout_s=5", "confirm.timeout_s=60"):
            run_set(pair)

    def test_out_of_range_is_refused_and_nothing_is_written(self):
        for pair in ("recognition.threshold=0.29", "recognition.threshold=0.91", "recognition.frames_needed=0",
                     "recognition.frames_needed=4", "timeout_ms=999", "timeout_ms=15001",
                     "confirm.timeout_s=4", "confirm.timeout_s=61", "detection.threshold=1.5",
                     "ir_liveness.min_pairs=0", "ir_liveness.min_pairs=4", "recognition.threshold=nan", "recognition.threshold=inf",
                     "ai_antispoof.threshold=-0.1"):
            self.assertIn("between", fails(pair), pair)
        self.assertFalse(os.path.exists(self.path))

    def test_wrong_types_are_refused(self):
        self.assertIn("number", fails("recognition.threshold=high"))
        self.assertIn("number", fails("recognition.frames_needed=1.5"))
        self.assertIn("number", fails("timeout_ms="))
        for value in ("yes", "1", "True", ""):
            self.assertIn("true or false", fails("disabled=" + value))
        self.assertIn("one of", fails("recognition.model=evil.onnx"))
        self.assertIn("one of", fails("recognition.model=../../etc/passwd"))
        self.assertIn("one of", fails("ai_antispoof.model=edgeface_s_gamma_05.onnx"))
        self.assertIn("Expected KEY=VALUE", fails("disabled"))
        self.assertFalse(os.path.exists(self.path))

    def test_unknown_keys_are_refused(self):
        for key in ("nope", "recognition.nope", "recognition", "confirm.services", "ignore_services",
                    "schema_version", "detection.model.x", ".", "RECOGNITION.threshold", "video.certainty"):
            self.assertIn("Unknown", fails(key + "=1"), key)
        self.assertFalse(os.path.exists(self.path))

    def test_one_bad_pair_means_no_change_at_all(self):
        run_set("confirm.timeout_s=20")
        before = self.saved()
        fails("disabled=true", "recognition.frames_needed=9")
        self.assertEqual(self.saved(), before)

    def test_camera_paths(self):
        # Pretend /dev/video* exist; the by-path names of real cameras (colons, dashes) must pass
        with mock.patch.object(helper.os.path, "exists", side_effect=lambda p: p.startswith("/dev/video") or os.path.lexists(p)):
            run_set("camera=/dev/video2")
            self.assertEqual(self.saved()["camera"], "/dev/video2")
            run_set("camera=/dev/video3")
            run_set("camera=")
            self.assertEqual(self.saved()["camera"], "")
            for bad in ("/etc/passwd", "video2", "/dev/../etc/passwd", "/dev/video2 --x", "/dev/video;rm", "/dev/nothere"):
                self.assertIn("evice", fails("camera=" + bad), bad)

    def test_unknown_keys_and_lists_in_the_file_survive(self):
        self.write("ignore_services: [sddm]\nfuture_option: 5\ndetection:\n  extra: x\n")
        run_set("disabled=true")
        data = self.saved()
        self.assertEqual(data["ignore_services"], ["sddm"])
        self.assertEqual(data["future_option"], 5)
        self.assertEqual(data["detection"]["extra"], "x")
        self.assertTrue(data["disabled"])

    def test_existing_mode_is_kept(self):
        self.write("disabled: false\n", mode=0o644)
        run_set("disabled=true")
        self.assertEqual(os.stat(self.path).st_mode & 0o777, 0o644)
        os.chmod(self.path, 0o640)
        run_set("disabled=false")
        self.assertEqual(os.stat(self.path).st_mode & 0o777, 0o640)

    def test_no_temporary_files_are_left_behind(self):
        run_set("disabled=true")
        run_set("disabled=false")
        self.assertEqual(os.listdir(self.dir), ["config.yaml"])

    def test_broken_file_is_not_overwritten(self):
        self.write("recognition: [unclosed\n")
        fails("disabled=true")
        with open(self.path) as f:
            self.assertEqual(f.read(), "recognition: [unclosed\n")

    def test_saving_is_idempotent(self):
        run_set("recognition.threshold=0.6")
        with open(self.path) as f:
            first = f.read()
        run_set("recognition.threshold=0.6")
        with open(self.path) as f:
            self.assertEqual(f.read(), first)


class ConfirmSettings(ConfigCase):
    """What confirm-broker asks (the settings it reads come from the same file)."""

    def test_enabled_for_the_listed_services_by_default(self):
        self.assertTrue(common.confirm_enabled("sudo"))
        self.assertTrue(common.confirm_enabled("polkit-1"))
        self.assertFalse(common.confirm_enabled("sddm"))

    def test_switched_off(self):
        run_set("confirm.enabled=false")
        self.assertFalse(common.confirm_enabled("sudo"))

    def test_master_switch_wins(self):
        run_set("disabled=true")
        self.assertFalse(common.confirm_enabled("sudo"))

    def test_ignored_service(self):
        self.write("ignore_services: [sudo]\n")
        self.assertFalse(common.confirm_enabled("sudo"))
        self.assertTrue(common.confirm_enabled("polkit-1"))

    def test_timeout(self):
        self.assertEqual(common.confirm_timeout(), 15)
        run_set("confirm.timeout_s=30")
        self.assertEqual(common.confirm_timeout(), 30)
        self.write("confirm:\n  timeout_s: banana\n")
        self.assertEqual(common.confirm_timeout(), 15)

    def test_unreadable_config_raises_so_the_broker_can_step_aside(self):
        self.write("confirm: [\n")
        with self.assertRaises(common.ConfigError):
            common.confirm_enabled("sudo")


if __name__ == "__main__":
    unittest.main()
