"""The Plasma lock screen gate: which input events count as "the user wants to unlock"."""

import os
import tempfile
import threading
import time
import unittest
from unittest import mock

from support import load_script

gate = load_script("input-gate")

EV_SYN, EV_KEY, EV_REL, EV_ABS = 0x00, 0x01, 0x02, 0x03
KEY_A = 30
KEY_ENTER = 28
BTN_LEFT = 0x110
BTN_TOOL_FINGER = 0x145
BTN_TOOL_DOUBLETAP = 0x14D
BTN_TOUCH = gate.BTN_TOUCH
REL_X, ABS_X = 0, 0


def new_state(settle_until=0.0):
    return {"settle_until": settle_until, "touch_down": None}


def event(etype, code, value, stamp=None):
    stamp = time.time() + 1 if stamp is None else stamp
    sec = int(stamp)
    return gate.EVENT.pack(sec, int((stamp - sec) * 1e6), etype, code, value)


class Triggers(unittest.TestCase):
    def trigger(self, etype, code, value, stamp=100.0, state=None):
        return gate.is_trigger(etype, code, value, stamp, state if state is not None else new_state())

    def test_key_press(self):
        self.assertTrue(self.trigger(EV_KEY, KEY_A, 1))
        self.assertTrue(self.trigger(EV_KEY, KEY_ENTER, 1))

    def test_mouse_click(self):
        self.assertTrue(self.trigger(EV_KEY, BTN_LEFT, 1))

    def test_release_of_the_key_that_woke_the_screen(self):
        # Its press happened before the gate started, so the release is all we see
        self.assertTrue(self.trigger(EV_KEY, KEY_A, 0))

    def test_key_repeat_is_not_a_new_press(self):
        self.assertFalse(self.trigger(EV_KEY, KEY_A, 2))

    def test_mouse_movement_is_ignored(self):
        self.assertFalse(self.trigger(EV_REL, REL_X, 5))
        self.assertFalse(self.trigger(EV_ABS, ABS_X, 300))
        self.assertFalse(self.trigger(EV_SYN, 0, 0))

    def test_resting_fingers_are_ignored(self):
        state = new_state()
        for code in (BTN_TOOL_FINGER, BTN_TOOL_DOUBLETAP, 0x140, 0x14F):
            self.assertFalse(self.trigger(EV_KEY, code, 1, state=state), hex(code))
            self.assertFalse(self.trigger(EV_KEY, code, 0, state=state), hex(code))

    def test_quick_touch_is_a_tap(self):
        state = new_state()
        self.assertFalse(self.trigger(EV_KEY, BTN_TOUCH, 1, stamp=100.0, state=state))
        self.assertTrue(self.trigger(EV_KEY, BTN_TOUCH, 0, stamp=100.1, state=state))
        self.assertIsNone(state["touch_down"])

    def test_long_touch_is_not_a_tap(self):
        state = new_state()
        self.trigger(EV_KEY, BTN_TOUCH, 1, stamp=100.0, state=state)
        self.assertFalse(self.trigger(EV_KEY, BTN_TOUCH, 0, stamp=100.0 + gate.TAP + 0.5, state=state))

    def test_touch_down_alone_is_not_a_trigger(self):
        self.assertFalse(self.trigger(EV_KEY, BTN_TOUCH, 1))

    def test_touch_release_without_a_press_is_ignored(self):
        self.assertFalse(self.trigger(EV_KEY, BTN_TOUCH, 0))

    def test_input_inside_the_settle_window_is_ignored(self):
        state = new_state(settle_until=105.0)
        self.assertFalse(self.trigger(EV_KEY, KEY_A, 1, stamp=104.9, state=state))
        self.assertFalse(self.trigger(EV_KEY, BTN_LEFT, 1, stamp=100.0, state=state))
        self.assertTrue(self.trigger(EV_KEY, KEY_A, 1, stamp=105.0, state=state))

    def test_tap_inside_the_settle_window_does_not_count_even_after_it(self):
        state = new_state(settle_until=105.0)
        self.trigger(EV_KEY, BTN_TOUCH, 1, stamp=104.9, state=state)
        self.assertIsNone(state["touch_down"])
        self.assertFalse(self.trigger(EV_KEY, BTN_TOUCH, 0, stamp=105.05, state=state))


class WaitForInput(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.release = os.path.join(tmp.name, "release")
        patcher = mock.patch.object(gate, "release_path", return_value=self.release)
        patcher.start()
        self.addCleanup(patcher.stop)
        self.read_fd, self.write_fd = os.pipe()
        os.set_blocking(self.read_fd, False)
        self.addCleanup(os.close, self.read_fd)
        self.addCleanup(os.close, self.write_fd)

    def send(self, *events):
        os.write(self.write_fd, b"".join(events))

    def test_a_key_press_lets_the_scan_go_ahead(self):
        self.send(event(EV_KEY, KEY_A, 1))
        self.assertEqual(gate.wait_for_input([self.read_fd], None), 0)

    def test_a_click_lets_the_scan_go_ahead(self):
        self.send(event(EV_KEY, BTN_LEFT, 1))
        self.assertEqual(gate.wait_for_input([self.read_fd], None), 0)

    def test_the_release_of_the_waking_key_lets_it_go_ahead(self):
        self.send(event(EV_KEY, KEY_A, 0))
        self.assertEqual(gate.wait_for_input([self.read_fd], None), 0)

    def test_movement_and_resting_fingers_are_skipped_until_a_real_input(self):
        self.send(event(EV_REL, REL_X, 3), event(EV_ABS, ABS_X, 10), event(EV_KEY, BTN_TOOL_FINGER, 1),
                  event(EV_SYN, 0, 0), event(EV_KEY, KEY_A, 1))
        self.assertEqual(gate.wait_for_input([self.read_fd], None), 0)

    def test_a_tap_lets_it_go_ahead(self):
        now = time.time() + 1
        self.send(event(EV_KEY, BTN_TOUCH, 1, now), event(EV_KEY, BTN_TOUCH, 0, now + 0.1))
        self.assertEqual(gate.wait_for_input([self.read_fd], None), 0)

    def test_input_right_after_locking_is_ignored_then_the_gate_waits(self):
        # The lock shortcut was pressed 0.1 s ago, so the next 1.9 s of input doesn't count
        self.send(event(EV_KEY, KEY_A, 0, time.time() + 0.2))
        timer = threading.Timer(0.6, lambda: self.touch_release())
        timer.start()
        self.addCleanup(timer.cancel)
        self.assertEqual(gate.wait_for_input([self.read_fd], 0.1), 1)

    def touch_release(self):
        with open(self.release, "w"):
            pass

    def test_a_password_unlock_releases_a_waiting_gate(self):
        self.send(event(EV_REL, REL_X, 3))
        timer = threading.Timer(0.5, self.touch_release)
        timer.start()
        self.addCleanup(timer.cancel)
        self.assertEqual(gate.wait_for_input([self.read_fd], None), 1)

    def test_an_old_release_file_does_not_release_a_new_gate(self):
        self.touch_release()
        os.utime(self.release, (time.time() - 60, time.time() - 60))
        self.send(event(EV_KEY, KEY_A, 1))
        self.assertEqual(gate.wait_for_input([self.read_fd], None), 0)

    def test_the_gate_stops_when_the_lock_screen_goes_away(self):
        with mock.patch.object(gate.os, "getppid", side_effect=[100, 200, 200, 200]):
            self.assertEqual(gate.wait_for_input([self.read_fd], None), 1)


class Main(unittest.TestCase):
    def test_release_writes_the_release_file(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, "gate-release")
            with mock.patch.object(gate, "release_path", return_value=path):
                self.assertEqual(gate.main(["--release"]), 0)
            self.assertTrue(os.path.exists(path))

    def test_release_path_is_per_user_and_named_for_the_project(self):
        self.assertEqual(gate.release_path(), f"/run/user/{os.getuid()}/linux-hello-camera-gate-release")

    def test_without_access_to_input_devices_the_scan_goes_ahead(self):
        with mock.patch.object(gate, "open_devices", return_value=[]):
            self.assertEqual(gate.main([]), 0)

    def test_main_waits_with_the_lock_age(self):
        with mock.patch.object(gate, "open_devices", return_value=[7]), \
                mock.patch.object(gate, "lock_age", return_value=1.5), \
                mock.patch.object(gate, "wait_for_input", return_value=0) as wait:
            self.assertEqual(gate.main([]), 0)
        wait.assert_called_once_with([7], 1.5)


if __name__ == "__main__":
    unittest.main()
