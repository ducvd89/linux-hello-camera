"""Face commands in settings-helper: id validation, remove, clear, enroll and test plumbing."""

import contextlib
import io
import json
import os
import tempfile
import unittest
from unittest import mock

from support import load_script

helper = load_script("settings-helper")


def run(*argv, env=None):
    out = io.StringIO()
    err = io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        with mock.patch.dict(os.environ, {"PKEXEC_UID": str(os.getuid())} if env is None else env, clear=True):
            code = helper.main(list(argv))
    return code, out.getvalue(), err.getvalue()


class FacesCase(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.tmp = tmp.name
        self.user = helper.pwd.getpwuid(os.getuid()).pw_name
        geteuid = mock.patch.object(helper.os, "geteuid", return_value=0)
        geteuid.start()
        self.addCleanup(geteuid.stop)

    def fake_engine(self, body):
        path = os.path.join(self.tmp, "engine")
        with open(path, "w") as f:
            f.write("#!/bin/sh\n" + body)
        os.chmod(path, 0o755)
        patcher = mock.patch.object(helper, "ENGINE", path)
        patcher.start()
        self.addCleanup(patcher.stop)


class RemoveFace(FacesCase):
    def setUp(self):
        super().setUp()
        # Answers like the engine and says what it was asked
        self.fake_engine('echo "{\\"result\\": \\"ok\\", \\"args\\": \\"$*\\"}"\n')

    def test_removes_one_entry_of_the_caller(self):
        code, out, _ = run("remove-face", "1759400000123")
        self.assertEqual(code, 0)
        self.assertEqual(json.loads(out)["args"], f"remove --username {self.user} --id 1759400000123")

    def test_only_digits_are_accepted(self):
        for face_id in ("../1", "1/2", "1.png", "", "-1", "1 2", "+1", "1e3", "a", "1;ls", "0x10", "1\n2",
                        "\u0661\u0662", "1" * 21, "../../etc/passwd", "*", " 1", ".", ".."):
            with self.assertRaises(SystemExit, msg=repr(face_id)):
                run("remove-face", face_id)

    def test_id_check(self):
        for good in ("1", "0", "1759400000123", "1" * 20):
            self.assertEqual(helper.check_face_id(good), good)

    def test_the_engine_is_never_run_for_a_bad_id(self):
        self.fake_engine('touch "$(dirname "$0")/ran"\n')
        with self.assertRaises(SystemExit):
            run("remove-face", "../1")
        self.assertFalse(os.path.exists(os.path.join(self.tmp, "ran")))

    def test_failure_is_passed_on(self):
        self.fake_engine('echo \'{"result": "error"}\'\nexit 1\n')
        code, out, _ = run("remove-face", "5")
        self.assertEqual(code, 1)

    def test_needs_exactly_one_argument(self):
        with self.assertRaises(SystemExit):
            run("remove-face")
        with self.assertRaises(SystemExit):
            run("remove-face", "1", "2")

    def test_needs_pkexec(self):
        with self.assertRaises(SystemExit):
            run("remove-face", "1", env={})
        with self.assertRaises(SystemExit):
            run("remove-face", "1", env={"PKEXEC_UID": "abc"})

    def test_root_is_refused(self):
        with self.assertRaises(SystemExit):
            run("remove-face", "1", env={"PKEXEC_UID": "0"})


class ClearFaces(FacesCase):
    def test_clears_the_callers_data_only(self):
        self.fake_engine('echo "{\\"result\\": \\"ok\\", \\"args\\": \\"$*\\"}"\n')
        code, out, _ = run("clear-faces")
        self.assertEqual(code, 0)
        self.assertEqual(json.loads(out)["args"], f"clear --username {self.user}")

    def test_takes_no_arguments(self):
        self.fake_engine("echo '{}'\n")
        with self.assertRaises(SystemExit):
            run("clear-faces", "someoneelse")


class EnginePlumbing(FacesCase):
    def test_enroll_streams_progress_for_the_caller(self):
        self.fake_engine('echo "ARGS $*"\nfor i in 1 2 3 4 5; do echo "PROGRESS $i 5"; done\necho "OK 5"\n')
        code, out, _ = run("enroll")
        self.assertEqual(code, 0)
        lines = out.splitlines()
        self.assertEqual(lines[0], f"ARGS enroll --username {self.user} --count 5")
        self.assertEqual(lines[1:], [f"PROGRESS {i} 5" for i in range(1, 6)] + ["OK 5"])

    def test_enroll_passes_on_failure(self):
        self.fake_engine('echo "ERR no_face"\nexit 1\n')
        code, out, _ = run("enroll")
        self.assertEqual(code, 1)
        self.assertEqual(out.strip(), "ERR no_face")

    def test_enroll_takes_no_arguments_and_no_other_user(self):
        self.fake_engine("echo OK\n")
        with self.assertRaises(SystemExit):
            run("enroll", "--username", "root")

    def test_test_prints_only_the_json_line(self):
        self.fake_engine('echo "noise"\necho \'{"result": "not_recognised", "best_score": 0.2}\'\nexit 1\n')
        code, out, _ = run("test")
        self.assertEqual(code, 1)
        self.assertEqual(json.loads(out)["result"], "not_recognised")

    def test_test_for_the_caller_only(self):
        self.fake_engine('echo "{\\"user\\": \\"$3\\"}"\n')
        code, out, _ = run("test")
        self.assertEqual(json.loads(out)["user"], self.user)

    def test_missing_engine_is_an_error(self):
        with mock.patch.object(helper, "ENGINE", os.path.join(self.tmp, "nope")):
            with self.assertRaises(FileNotFoundError):
                run("test")


if __name__ == "__main__":
    unittest.main()
