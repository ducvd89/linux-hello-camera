"""confirm-hook: approves only when the dialog says yes, never prints, refuses on any problem."""

import contextlib
import io
import os
import socket
import tempfile
import threading
import unittest
from unittest import mock

from support import load_script

import linux_hello_camera_common as common

hook = load_script("confirm-hook")

GOOD = ["--user", "someone", "--service", "sudo", "--pam-pid", "4242"]


def run_hook(argv):
    out, err = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        code = hook.main(argv)
    assert out.getvalue() == "" and err.getvalue() == "", "confirm-hook must not print"
    return code


class Arguments(unittest.TestCase):
    def test_parse(self):
        self.assertEqual(hook.parse(GOOD), {"user": "someone", "service": "sudo", "pam-pid": "4242"})

    def test_bad_command_lines_are_refused_silently(self):
        for argv in ([], GOOD[:-1], GOOD + ["--x", "1"], ["--service", "sudo", "--user", "a", "--pam-pid", "1"],
                     ["--user", "a", "--service", "sudo", "--pam-pid", "x"]):
            self.assertEqual(run_hook(argv), 1, argv)

    def test_other_services_and_odd_pids_are_refused(self):
        with mock.patch.object(common, "trusted_socket", side_effect=AssertionError("must not get that far")):
            for service in ("sddm", "gdm-password", "kde-fingerprint", ""):
                self.assertEqual(run_hook(["--user", "a", "--service", service, "--pam-pid", "4242"]), 1)
            for pid in ("0", "1", "-5", "4242;ls", ""):
                self.assertEqual(run_hook(["--user", "a", "--service", "sudo", "--pam-pid", pid]), 1)


class WithBroker(unittest.TestCase):
    """A broker socket that answers like confirm-broker does."""

    def serve(self, answer):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        path = os.path.join(tmp.name, "s.sock")
        server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        server.bind(path)
        server.listen(1)
        self.addCleanup(server.close)
        self.received = []

        def answer_once():
            conn, _ = server.accept()
            self.received.append(conn.makefile("rb").readline())
            if answer is not None:
                conn.sendall(answer)
            conn.close()
        thread = threading.Thread(target=answer_once, daemon=True)
        thread.start()
        patcher = mock.patch.object(common, "trusted_socket", return_value=path)
        patcher.start()
        self.addCleanup(patcher.stop)
        self.addCleanup(thread.join, 2)

    def test_yes_approves_and_the_timeout_is_sent(self):
        self.serve(b"yes\n")
        with mock.patch.object(common, "confirm_timeout", return_value=20):
            self.assertEqual(run_hook(GOOD), 0)
        self.assertEqual(self.received, [b"matched 20\n"])

    def test_polkit_works_too(self):
        self.serve(b"yes\n")
        with mock.patch.object(common, "confirm_timeout", return_value=15):
            self.assertEqual(run_hook(["--user", "a", "--service", "polkit-1", "--pam-pid", "99"]), 0)

    def test_no_refuses(self):
        self.serve(b"no\n")
        self.assertEqual(run_hook(GOOD), 1)

    def test_anything_else_refuses(self):
        for answer in (b"", b"YES\n", b"yes please\n", b"\n", None):
            self.serve(answer)
            self.assertEqual(run_hook(GOOD), 1, answer)


class WithoutBroker(unittest.TestCase):
    def setUp(self):
        for name, value in (("trusted_socket", None), ("confirm_timeout", 15)):
            patcher = mock.patch.object(common, name, return_value=value)
            patcher.start()
            self.addCleanup(patcher.stop)
        pw = mock.patch.object(hook.pwd, "getpwnam", return_value=mock.Mock(pw_uid=1000))
        pw.start()
        self.addCleanup(pw.stop)

    def test_no_desktop_session_refuses(self):
        with mock.patch.object(common, "desktop_env", return_value=None), \
                mock.patch.object(hook.subprocess, "run", side_effect=AssertionError("no dialog without a desktop")):
            self.assertEqual(run_hook(GOOD), 1)

    def test_opens_the_dialog_itself_and_follows_its_exit_code(self):
        for code, expected in ((0, 0), (1, 1), (2, 1)):
            with mock.patch.object(common, "desktop_env", return_value={"WAYLAND_DISPLAY": "w"}), \
                    mock.patch.object(common, "sudo_command", return_value="sudo true"), \
                    mock.patch.object(common, "as_user", return_value={}), \
                    mock.patch.object(hook.subprocess, "run", return_value=mock.Mock(returncode=code)) as run:
                self.assertEqual(run_hook(GOOD), expected)
            argv = run.call_args.args[0]
            self.assertEqual(argv[0], common.DIALOG)
            self.assertEqual(argv[argv.index("--kind") + 1], "sudo")
            self.assertEqual(argv[argv.index("--detail") + 1], "sudo true")
            self.assertEqual(run.call_args.kwargs["stdout"], hook.subprocess.DEVNULL)

    def test_a_crash_refuses(self):
        with mock.patch.object(common, "desktop_env", side_effect=RuntimeError("boom")):
            self.assertEqual(run_hook(GOOD), 1)

    def test_a_dialog_timeout_refuses(self):
        with mock.patch.object(common, "desktop_env", return_value={"DISPLAY": ":0"}), \
                mock.patch.object(common, "sudo_command", return_value=""), \
                mock.patch.object(common, "as_user", return_value={}), \
                mock.patch.object(hook.subprocess, "run",
                                  side_effect=hook.subprocess.TimeoutExpired("confirm-dialog", 20)):
            self.assertEqual(run_hook(GOOD), 1)


class HasFaces(unittest.TestCase):
    def test_looks_for_the_users_credential_file(self):
        with tempfile.TemporaryDirectory() as tmp, mock.patch.object(common, "TEMPLATES_DIR", tmp):
            self.assertFalse(common.has_faces("someone"))
            open(os.path.join(tmp, "someone.cred"), "w").close()
            self.assertTrue(common.has_faces("someone"))
            self.assertFalse(common.has_faces("other"))

    def test_every_template_name_counts(self):
        for ext in (".json", ".tpm.cred", ".tpm-sb.cred", ".cred"):
            with tempfile.TemporaryDirectory() as tmp, mock.patch.object(common, "TEMPLATES_DIR", tmp):
                open(os.path.join(tmp, "someone" + ext), "w").close()
                self.assertTrue(common.has_faces("someone"), ext)
                self.assertFalse(common.has_faces("other"), ext)

    def test_other_extensions_do_not_count(self):
        with tempfile.TemporaryDirectory() as tmp, mock.patch.object(common, "TEMPLATES_DIR", tmp):
            for ext in (".txt", ".tpm", ".cred.bak", ".tpm-sb", ".png"):
                open(os.path.join(tmp, "someone" + ext), "w").close()
            self.assertFalse(common.has_faces("someone"))

    def test_odd_names_are_never_found(self):
        with tempfile.TemporaryDirectory() as tmp, mock.patch.object(common, "TEMPLATES_DIR", tmp):
            open(os.path.join(tmp, "x.cred"), "w").close()
            open(os.path.join(tmp, "x.json"), "w").close()
            open(os.path.join(tmp, "x.tpm-sb.cred"), "w").close()
            os.mkdir(os.path.join(tmp, "sub"))
            open(os.path.join(tmp, "sub", "x.cred"), "w").close()
            open(os.path.join(tmp, "sub", "x.json"), "w").close()
            for name in ("", "../x", "sub/x", ".", ".hidden"):
                self.assertFalse(common.has_faces(name), name)

    def test_the_templates_folder_is_where_the_engine_puts_them(self):
        self.assertEqual(common.TEMPLATES_DIR, "/var/lib/linux-hello-camera/templates")


class SocketTrust(unittest.TestCase):
    def test_broker_sockets_have_their_own_private_folder(self):
        # /run/linux-hello-camera itself is open to everyone (engine.sock), so not there
        self.assertEqual(common.SOCKET_DIR, "/run/linux-hello-camera/confirm")
        self.assertEqual(common.socket_path(42), "/run/linux-hello-camera/confirm/42.sock")

    def test_a_missing_socket_is_not_trusted(self):
        with mock.patch.object(common, "SOCKET_DIR", "/nonexistent/linux-hello-camera"):
            self.assertIsNone(common.trusted_socket(1234))

    def test_a_socket_made_by_a_normal_user_is_not_trusted(self):
        with tempfile.TemporaryDirectory() as tmp:
            os.chmod(tmp, 0o700)
            path = os.path.join(tmp, "1234.sock")
            server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            self.addCleanup(server.close)
            server.bind(path)
            with mock.patch.object(common, "SOCKET_DIR", tmp):
                # Owned by this (non-root) user, so root's checks must refuse it
                self.assertEqual(common.trusted_socket(1234), None if os.getuid() != 0 else path)


if __name__ == "__main__":
    unittest.main()
