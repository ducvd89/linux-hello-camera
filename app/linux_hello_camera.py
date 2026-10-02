#!/usr/bin/python3
"""Linux Hello Camera - a GTK4/libadwaita app to set up face unlock with an IR camera."""

import glob
import grp
import json
import os
import pwd
import re
import subprocess
import sys
import threading
import time
from collections import deque

import gi

gi.require_version("Gtk", "4.0")
gi.require_version("Adw", "1")
gi.require_version("Gdk", "4.0")
from gi.repository import Adw, Gdk, Gio, GLib, Gtk  # noqa: E402

sys.path.insert(0, os.path.dirname(os.path.realpath(__file__)))
import linux_hello_camera_common as common  # noqa: E402

APP_ID = "io.github.linux_hello_camera"
VERSION = "0.9.1"
CONFIG_PATH = common.CONFIG_PATH
ENGINE = common.ENGINE
INSTALLED_HELPER = common.LIB_DIR + "/settings-helper"
LOCAL_HELPER = os.path.join(os.path.dirname(os.path.realpath(__file__)), "settings-helper")
HELPER = INSTALLED_HELPER if os.path.exists(INSTALLED_HELPER) else LOCAL_HELPER
USER = pwd.getpwuid(os.getuid()).pw_name

# One switch per feature. Each covers every installed PAM service for that feature, so the
# login switch works the same with GDM (GNOME), SDDM and Plasma Login Manager (KDE).
# GNOME's lock screen shares gdm-password with its login screen; KDE's has its own service.
PAM_FEATURES = [
    ("sudo", ("sudo",)),
    ("login", ("gdm-password", "sddm", "plasmalogin")),
    ("lock", ("kde-fingerprint",)),
    ("polkit", ("polkit-1",)),
]
KDE_LOGIN_SERVICES = ("sddm", "plasmalogin")
PAM_MODULE = "pam_linux_hello_camera.so"
# Extra piece each service needs besides the PAM module: the input gate on the Plasma lock
# screen, the early confirmation dialog for sudo and admin prompts
PAM_REQUIRES = {"kde-fingerprint": common.LIB_DIR + "/input-gate",
                "sudo": common.LIB_DIR + "/confirm-broker",
                "polkit-1": common.LIB_DIR + "/confirm-broker"}
PAM_DIRS = ("/etc/pam.d", "/usr/lib/pam.d")
# Services that older versions of this app used for the same switch. They show as off
# with a hint; turning the switch on moves face unlock over (the helper removes the old entry).
PAM_LEGACY = {"kde-fingerprint": ("kde",)}

RECOGNITION_MODELS = [("edgeface_s_gamma_05.onnx", "EdgeFace S (more accurate)"),
                      ("edgeface_xs_gamma_06.onnx", "EdgeFace XS (faster)")]

# What the engine's `test` command reports (its JSON "result") -> dialog title and text
TEST_RESULTS = {
    "ok": ("Face recognised", "Linux Hello Camera identified you. Face unlock is working."),
    "not_recognised": ("Not recognised", "No matching face was found before the timeout. Try again, add another face, or lower the match threshold in Settings."),
    "no_face": ("No face found", "The camera didn't see a face. Look straight into the camera and try again."),
    "liveness_failed": ("Looks like a photo or screen", "The check for a real face using the IR light failed. Look at the camera directly, or turn the check off in Settings if this camera has no flashing IR light."),
    "not_enrolled": ("No face added", "Add your face first."),
    "camera_unavailable": ("Camera unavailable", "Could not open the configured camera. Pick a camera on the Camera page."),
    "too_dark": ("Image too dark", "The camera image is too dark. Check that the IR light is on."),
}


# ---------------------------------------------------------------- helpers

def engine_problem():
    """(title, text, command to show) when the app can't work yet, else None."""
    if common.yaml is None:
        return ("python-yaml is missing", "Install it with the command below, then reopen this app.",
                "sudo pacman -S python-yaml")
    if not os.path.exists(ENGINE) or not os.path.exists(CONFIG_PATH):
        return ("Linux Hello Camera is not installed",
                "Run the installer from the project folder, then reopen this app.", "./install.sh")
    return None


def read_config():
    """The config with defaults filled in. Falls back to the defaults if it can't be read."""
    try:
        return common.load_config(CONFIG_PATH)
    except common.ConfigError:
        return common.merge(common.DEFAULTS, {})


ENCRYPTION_TEXT = {
    "tpm-sb": "Locked to this PC's security chip and Secure Boot.",
    "tpm": "Locked to this PC's security chip (TPM).",
    "none": "Not encrypted: stored as templates, never photos.",
}


def pam_available(service):
    return any(os.path.exists(os.path.join(d, service)) for d in PAM_DIRS)


def in_input_group():
    """the input gate needs to read /dev/input to see key presses and clicks."""
    try:
        return grp.getgrnam("input").gr_gid in os.getgroups()
    except KeyError:
        return False


def feature_text(feature, services):
    """Title and subtitle for a feature switch, worded for the login managers installed."""
    if feature == "sudo":
        return "sudo", "Terminal commands run with sudo"
    if feature == "polkit":
        return "Administrator prompts", "Password dialogs from apps and system settings"
    if feature == "lock":
        if not in_input_group():
            return "Lock screen", ("Starts as soon as the mouse moves. Add yourself to the “input” group "
                                   "to wait for a key press or click instead")
        return "Lock screen", "Looks for your face when you press a key or click"
    gnome = "gdm-password" in services
    kde = any(s in services for s in KDE_LOGIN_SERVICES)
    separate_lock = pam_available("kde-fingerprint")
    title = "Login and lock screen" if gnome and not separate_lock else "Login screen"
    subtitle = "Signing in after you start the computer"
    if gnome and not separate_lock:
        subtitle = "Signing in and unlocking the screen"
    elif gnome:
        subtitle += ", and the GNOME lock screen"
    if kde:
        subtitle += (". On SDDM and Plasma Login, press Enter with an empty password"
                     if gnome else ". Press Enter with an empty password to scan")
    return title, subtitle




def pam_lines(service):
    try:
        with open(os.path.join("/etc/pam.d", service)) as f:
            return [l for l in f if not l.strip().startswith("#")]
    except OSError:
        return []


def pam_has_module(service):
    return any(PAM_MODULE in l for l in pam_lines(service))


def pam_enabled(service):
    """Face unlock is set up the way this version of the app does it."""
    lines = pam_lines(service)
    return any(PAM_MODULE in l for l in lines) and \
        (service not in PAM_REQUIRES or any(PAM_REQUIRES[service] in l for l in lines))


def list_cameras():
    """Return [(path, title, subtitle, is_ir)] for every capture-capable V4L2 node."""
    by_path = {}
    for link in sorted(glob.glob("/dev/v4l/by-path/*")):
        target = os.path.realpath(link)
        # Prefer the shorter "usb-" name over the duplicated "usbv2-" one
        if target not in by_path or "usbv2" in by_path[target]:
            by_path[target] = link

    cams = []
    for dev in sorted(glob.glob("/dev/video*"), key=lambda d: int(re.sub(r"\D", "", d) or 0)):
        try:
            out = subprocess.run(["v4l2-ctl", "-d", dev, "--info", "--list-formats"],
                                 capture_output=True, text=True, timeout=3).stdout
        except (OSError, subprocess.TimeoutExpired):
            continue
        formats = re.findall(r"\[\d+\]: '(\w+)'", out)
        if not formats:
            continue  # metadata node, not a capture device
        card = re.search(r"Card type\s*:\s*(.+)", out)
        card = card.group(1).strip() if card else dev
        is_ir = "GREY" in formats or re.search(r"\bIR\b|infrared", card, re.I) is not None
        title = ("IR camera" if is_ir else "Camera") + f" ({os.path.basename(dev)})"
        cams.append((by_path.get(dev, dev), title, f"{card} · {', '.join(formats)}", is_ir))
    return cams


def device_size(dev):
    try:
        out = subprocess.run(["v4l2-ctl", "-d", dev, "--get-fmt-video"],
                             capture_output=True, text=True, timeout=3).stdout
        w, h = re.search(r"Width/Height\s*:\s*(\d+)/(\d+)", out).groups()
        return int(w), int(h)
    except (OSError, AttributeError, subprocess.TimeoutExpired):
        return 640, 480


class Helper:
    """Runs the privileged helper through pkexec and streams its output."""

    def __init__(self):
        self.busy = False

    def run(self, args, on_line=None, on_done=None):
        try:
            proc = Gio.Subprocess.new(["pkexec", HELPER] + list(args),
                                      Gio.SubprocessFlags.STDOUT_PIPE | Gio.SubprocessFlags.STDERR_MERGE)
        except GLib.Error as e:
            if on_done:
                GLib.idle_add(on_done, -1, e.message)
            return
        self.busy = True
        stream = Gio.DataInputStream.new(proc.get_stdout_pipe())
        output = []

        def read_line(src, res):
            try:
                line, _ = src.read_line_finish_utf8(res)
            except GLib.Error:
                line = None
            if line is None:
                proc.wait_async(None, finished)
                return
            output.append(line)
            if on_line and line.strip():
                on_line(line.strip())
            src.read_line_async(GLib.PRIORITY_DEFAULT, None, read_line)

        def finished(p, res):
            p.wait_finish(res)
            self.busy = False
            code = p.get_exit_status() if p.get_if_exited() else -1
            if on_done:
                on_done(code, "\n".join(output))

        stream.read_line_async(GLib.PRIORITY_DEFAULT, None, read_line)



# ---------------------------------------------------------------- camera preview

def strobe_state(means):
    """Whether the brightness of consecutive frames keeps flipping between lit and unlit.
    None while there are too few frames to tell."""
    if len(means) < 8:
        return None
    if max(means) - min(means) < 8:
        return False
    mid = (max(means) + min(means)) / 2
    lit = [m > mid for m in means]
    flips = sum(a != b for a, b in zip(lit, lit[1:]))
    return flips >= 0.7 * (len(lit) - 1)


class CameraPreview(Gtk.Box):
    """Live greyscale preview with the darkness level and whether the IR light is strobing."""

    WIDTH = 640

    def __init__(self):
        super().__init__()
        self.picture = Gtk.Picture(content_fit=Gtk.ContentFit.CONTAIN, can_shrink=True, hexpand=True)
        self.picture.set_size_request(320, 200)
        self.append(self.picture)
        self.add_css_class("card")
        self.set_overflow(Gtk.Overflow.HIDDEN)

        self.proc = None
        self.thread = None
        self.running = False
        self.size = (self.WIDTH, 400)
        self.on_stats = None
        self.device = None
        self.generation = 0
        self.watchdog = 0
        self.restarts = []
        self.last_frame = 0.0

    def start(self, device):
        self.stop()
        self.device = device
        self.restarts = []
        self._spawn()

    def _spawn(self):
        w0, h0 = device_size(self.device)
        w = min(self.WIDTH, w0)
        h = max(2, round(h0 * w / w0 / 2) * 2)
        self.size = (w, h)
        try:
            self.proc = subprocess.Popen(
                ["ffmpeg", "-loglevel", "error", "-f", "v4l2", "-i", self.device,
                 "-vf", f"scale={w}:{h}", "-pix_fmt", "gray", "-f", "rawvideo", "-"],
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, stdin=subprocess.DEVNULL)
        except OSError as e:
            self._report(None, None, f"Could not start ffmpeg: {e}")
            return
        self.running = True
        self.generation += 1
        self.last_frame = time.monotonic()
        self.thread = threading.Thread(target=self._loop, args=(self.proc, self.generation), daemon=True)
        self.thread.start()
        self.watchdog = GLib.timeout_add(1000, self._watchdog)

    def stop(self):
        self.running = False
        if self.watchdog:
            GLib.source_remove(self.watchdog)
            self.watchdog = 0
        if self.proc:
            # ffmpeg ignores SIGTERM while blocked on a full pipe; it has nothing to flush anyway
            self.proc.kill()
            self.proc.wait()
            self.proc = None

    def _restart(self, generation):
        """Reopen the camera. Some IR cameras stop streaming after a few seconds."""
        if not self.running or generation != self.generation:
            return False
        now = time.monotonic()
        self.restarts = [t for t in self.restarts if now - t < 10] + [now]
        if len(self.restarts) > 3:
            self.stop()
            self._report(None, None, "The camera keeps stopping its stream")
            return False
        self.stop()
        self._spawn()
        return False

    def _watchdog(self):
        if not self.running:
            self.watchdog = 0
            return False
        if time.monotonic() - self.last_frame > 3:
            self.watchdog = 0
            self._restart(self.generation)
            return False
        return True

    @staticmethod
    def _darkness(frame):
        # Share of pixels in the lowest 1/8 of the grey levels. A rough estimate of how dark
        # the picture is; the engine looks at the face area only.
        dark = len(frame) - len(frame.translate(None, bytes(range(32))))
        return dark * 100 / len(frame)

    @staticmethod
    def _brightness(frame):
        sample = frame[::16]
        return sum(sample) / len(sample)

    def _loop(self, proc, generation):
        w, h = self.size
        n = w * h
        flat = 0
        last_shown = 0.0
        means = deque(maxlen=12)
        # Only the reader thread touches stdout; the process itself is reaped by stop()
        while self.running and generation == self.generation:
            frame = proc.stdout.read(n)
            if not frame or len(frame) < n:
                break
            self.last_frame = time.monotonic()
            # A perfectly uniform frame means the camera stopped sending real images
            if frame.count(frame[:1]) == n:
                flat += 1
                if flat >= 3:
                    GLib.idle_add(self._restart, generation)
                    return
                continue
            flat = 0
            means.append(self._brightness(frame))
            darkness = self._darkness(frame)
            # IR emitters strobe, so every other frame can be black. The engine skips
            # those too; hide them unless no lit frame has arrived for a while.
            if darkness >= 99 and time.monotonic() - last_shown < 1.0:
                continue
            last_shown = time.monotonic()
            GLib.idle_add(self._show, frame, (w, h), darkness, strobe_state(list(means)))
        if self.running and generation == self.generation:
            GLib.idle_add(self._restart, generation)

    def _show(self, frame, size, darkness, strobing):
        if not self.running or size != self.size:
            return False
        w, h = size
        tex = Gdk.MemoryTexture.new(w, h, Gdk.MemoryFormat.G8, GLib.Bytes.new(frame), w)
        self.picture.set_paintable(tex)
        self._report(darkness, strobing, None)
        return False

    def _report(self, darkness, strobing, error):
        if self.on_stats:
            self.on_stats(darkness, strobing, error)
        return False


# ---------------------------------------------------------------- window

class LinuxHelloCameraWindow(Adw.ApplicationWindow):
    def __init__(self, app):
        super().__init__(application=app, title="Linux Hello Camera", default_width=760, default_height=780)
        self.helper = Helper()
        self.config = read_config()
        self.pending = {}
        self._syncing = False

        self.toasts = Adw.ToastOverlay()
        self.set_content(self.toasts)

        problem = engine_problem()
        if problem:
            self._build_not_installed(*problem)
            return

        self.stack = Adw.ViewStack()
        header = Adw.HeaderBar()
        switcher = Adw.ViewSwitcher(stack=self.stack, policy=Adw.ViewSwitcherPolicy.WIDE)
        header.set_title_widget(switcher)
        about = Gtk.Button(icon_name="help-about-symbolic", tooltip_text="About")
        about.connect("clicked", self._on_about)
        header.pack_end(about)

        bottom = Adw.ViewSwitcherBar(stack=self.stack)
        view = Adw.ToolbarView()
        view.add_top_bar(header)
        view.set_content(self.stack)
        view.add_bottom_bar(bottom)
        self.toasts.set_child(view)

        bp = Adw.Breakpoint.new(Adw.BreakpointCondition.parse("max-width: 550sp"))
        bp.add_setter(bottom, "reveal", True)
        bp.add_setter(header, "title-widget", Adw.WindowTitle(title="Linux Hello Camera"))
        self.add_breakpoint(bp)

        self.stack.add_titled_with_icon(self._build_overview(), "overview", "Face Unlock", "avatar-default-symbolic")
        self.stack.add_titled_with_icon(self._build_camera(), "camera", "Camera", "camera-web-symbolic")
        self.stack.add_titled_with_icon(self._build_settings(), "settings", "Settings", "emblem-system-symbolic")
        self.stack.connect("notify::visible-child-name", self._on_page_changed)
        self.connect("close-request", lambda *_: self.preview.stop())

        self.refresh()

    # ------------------------------------------------ not installed

    def _build_not_installed(self, title, text, command):
        page = Adw.StatusPage(icon_name="camera-web-symbolic", title=title, description=text)
        box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=12, halign=Gtk.Align.CENTER)
        cmd = Gtk.Label(label=command, selectable=True, css_classes=["monospace", "title-3"])
        box.append(cmd)
        retry = Gtk.Button(label="Check Again", css_classes=["pill", "suggested-action"], halign=Gtk.Align.CENTER)
        retry.connect("clicked", lambda *_: self._restart())
        box.append(retry)
        page.set_child(box)
        view = Adw.ToolbarView()
        view.add_top_bar(Adw.HeaderBar())
        view.set_content(page)
        self.toasts.set_child(view)

    def _restart(self):
        app = self.get_application()
        self.destroy()
        LinuxHelloCameraWindow(app).present()

    # ------------------------------------------------ overview page

    def _build_overview(self):
        page = Adw.PreferencesPage()

        self.banner = Adw.Banner(revealed=False)
        self.banner.connect("button-clicked", lambda *_: self.stack.set_visible_child_name("camera"))

        status = Adw.PreferencesGroup()
        self.master_row = Adw.SwitchRow(title="Face authentication",
                                        subtitle="Turn face unlock on or off everywhere")
        self.master_row.connect("notify::active", self._on_master_toggled)
        status.add(self.master_row)
        page.add(status)

        pam_group = Adw.PreferencesGroup(title="Use Face Unlock For",
                                         description="You can always fall back to your password.")
        self.pam_rows = {}
        for feature, candidates in PAM_FEATURES:
            services = tuple(s for s in candidates if pam_available(s))
            if not services:
                continue
            title, subtitle = feature_text(feature, services)
            row = Adw.SwitchRow(title=title, subtitle=subtitle)
            row.connect("notify::active", self._on_pam_toggled, services)
            pam_group.add(row)
            self.pam_rows[feature] = (row, services, subtitle)
        page.add(pam_group)

        confirm_group = Adw.PreferencesGroup()
        self.confirm_row = Adw.SwitchRow(
            title="Confirm sudo and admin prompts",
            subtitle="After recognising your face, wait for you to click Continue, like Windows Hello")
        self.confirm_row.connect("notify::active", self._on_confirm_toggled)
        confirm_group.add(self.confirm_row)
        page.add(confirm_group)

        self.models_group = Adw.PreferencesGroup(title="Face Models",
                                                 description=self._faces_description(None))
        add_btn = Gtk.Button(child=Adw.ButtonContent(icon_name="list-add-symbolic", label="Add"),
                             css_classes=["flat"])
        add_btn.connect("clicked", self._on_add_clicked)
        self.models_group.set_header_suffix(add_btn)
        # One switch at the top of the group, then the entries
        self.crypt_row = Adw.SwitchRow(title="Lock face data to this PC's security chip", subtitle="—")
        self.crypt_row.connect("notify::active", self._on_crypt_toggled)
        self.models_group.add(self.crypt_row)
        self.model_rows = []
        self._engine_generation = {}
        self.encryption_status = common.local_status(self.config)
        page.add(self.models_group)

        test_group = Adw.PreferencesGroup()
        test_row = Adw.ActionRow(title="Test recognition",
                                 subtitle="Look at the camera and check that it knows you")
        test_btn = Gtk.Button(label="Test", valign=Gtk.Align.CENTER, css_classes=["suggested-action"])
        test_btn.connect("clicked", self._on_test_clicked)
        test_row.add_suffix(test_btn)
        test_row.set_activatable_widget(test_btn)
        test_group.add(test_row)

        self.clear_row = Adw.ActionRow(title="Remove all face pictures")
        clear_btn = Gtk.Button(label="Remove All", valign=Gtk.Align.CENTER, css_classes=["destructive-action"])
        clear_btn.connect("clicked", self._on_clear_clicked)
        self.clear_row.add_suffix(clear_btn)
        test_group.add(self.clear_row)
        page.add(test_group)

        box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL)
        box.append(self.banner)
        page.set_vexpand(True)
        box.append(page)
        return box

    # ------------------------------------------------ camera page

    def _build_camera(self):
        page = Adw.PreferencesPage()
        group = Adw.PreferencesGroup(
            title="Camera",
            description="Face unlock needs an IR camera. The preview shows what it sees; "
                        "the IR light flashes, so some pictures are dark.")
        self.cam_model = Gtk.StringList()
        self.cameras = []
        self.cam_row = Adw.ComboRow(title="Device", model=self.cam_model)
        self.cam_row.connect("notify::selected", self._on_camera_selected)
        group.add(self.cam_row)

        self.cam_info = Adw.ActionRow(title="In use by Linux Hello Camera", subtitle="—")
        self.cam_info.add_css_class("property")
        self.use_cam_btn = Gtk.Button(label="Use This Camera", valign=Gtk.Align.CENTER,
                                      css_classes=["suggested-action"])
        self.use_cam_btn.connect("clicked", self._on_use_camera)
        self.cam_info.add_suffix(self.use_cam_btn)
        group.add(self.cam_info)
        page.add(group)

        preview_group = Adw.PreferencesGroup()
        self.preview = CameraPreview()
        self.preview.on_stats = self._on_preview_stats
        self.preview.set_margin_bottom(12)
        preview_group.add(self.preview)

        self.dark_bar = Gtk.LevelBar(min_value=0, max_value=100, mode=Gtk.LevelBarMode.CONTINUOUS)
        for name in ("low", "high", "full"):
            self.dark_bar.remove_offset_value(name)
        self.dark_row = Adw.ActionRow(title="Darkness", subtitle="—")
        self.dark_bar.set_hexpand(True)
        self.dark_bar.set_valign(Gtk.Align.CENTER)
        self.dark_bar.set_size_request(160, -1)
        self.dark_row.add_suffix(self.dark_bar)
        self.strobe_row = Adw.ActionRow(title="IR light flashing", subtitle="—")
        stats = Adw.PreferencesGroup()
        stats.add(self.dark_row)
        stats.add(self.strobe_row)

        self.preview_toggle = Gtk.ToggleButton(label="Preview", active=False, valign=Gtk.Align.CENTER)
        self.preview_toggle.connect("toggled", self._on_preview_toggled)
        preview_group.set_header_suffix(self.preview_toggle)
        preview_group.set_title("Live Preview")
        page.add(preview_group)
        page.add(stats)
        return page

    # ------------------------------------------------ settings page

    def _build_settings(self):
        self.setting_widgets = {}
        page = Adw.PreferencesPage()

        rec = Adw.PreferencesGroup(title="Recognition")
        self._spin(rec, "recognition.threshold", "Match threshold",
                   "Higher is stricter. How alike two faces must be", 0.30, 0.90, 0.01, 2)
        self._spin(rec, "recognition.frames_needed", "Frames needed",
                   "Camera pictures that must match, out of the last 3", 1, 3, 1, 0)
        self._spin(rec, "timeout_ms", "Timeout", "Seconds to look for a face", 1, 15, 1, 0, factor=1000)
        self._combo(rec, "recognition.model", "Model", [m for m, _ in RECOGNITION_MODELS],
                    [label for _, label in RECOGNITION_MODELS])
        self._spin(rec, "detection.threshold", "Face detection threshold",
                   "How sure the camera must be that it sees a face", 0.10, 0.95, 0.05, 2)
        page.add(rec)

        live = Adw.PreferencesGroup(title="Liveness",
                                    description="Keeps a photo or a screen from unlocking your computer.")
        self._switch(live, "ir_liveness.enabled", "Check for a real face using the IR light",
                     "Looks at how the face reflects the camera's flashing IR light")
        advanced = Adw.ExpanderRow(title="Advanced", subtitle="Only change these if real faces keep failing")
        self._spin(advanced, "ir_liveness.min_pairs", "Pictures to check",
                   "Lit and unlit picture pairs that must pass", 1, 3, 1, 0)
        self._spin(advanced, "ir_liveness.min_face_gain", "Minimum face brightening",
                   "How much brighter the IR light makes the face (grey levels)", 0, 255, 1, 0)
        self._spin(advanced, "ir_liveness.min_gain_ratio", "Minimum face-to-background ratio",
                   "How much more the face brightens than what is behind it", 1.0, 10.0, 0.1, 1)
        live.add(advanced)
        self._switch(live, "ai_antispoof.enabled", "AI anti-spoofing (for colour cameras)",
                     "Experimental. Made for colour pictures, so leave it off with an IR camera")
        page.add(live)

        beh = Adw.PreferencesGroup(title="Behaviour")
        self._switch(beh, "abort_if_lid_closed", "Skip when the lid is closed", None)
        self._switch(beh, "abort_if_ssh", "Skip over SSH", "Don't use face unlock for remote sessions")
        self._spin(beh, "confirm.timeout_s", "Confirmation timeout",
                   "Seconds to wait for Continue before sudo or an admin prompt falls back to the password",
                   5, 60, 1, 0)
        page.add(beh)

        self.apply_banner = Adw.Banner(title="You have unsaved changes", button_label="Apply")
        self.apply_banner.connect("button-clicked", self._on_apply_settings)

        box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL)
        box.append(self.apply_banner)
        page.set_vexpand(True)
        box.append(page)
        return box

    @staticmethod
    def _add_to(container, row):
        (container.add_row if isinstance(container, Adw.ExpanderRow) else container.add)(row)

    def _spin(self, group, key, title, subtitle, lo, hi, step, digits, factor=1):
        """A number row. FACTOR converts the shown value to the config's (seconds to ms)."""
        row = Adw.SpinRow.new_with_range(lo, hi, step)
        row.set_title(title)
        if subtitle:
            row.set_subtitle(subtitle)
        row.set_digits(digits)
        row.connect("notify::value", self._on_setting_changed, key)
        self._add_to(group, row)
        self.setting_widgets[key] = ("spin", row, digits, factor)

    def _switch(self, group, key, title, subtitle):
        row = Adw.SwitchRow(title=title)
        if subtitle:
            row.set_subtitle(subtitle)
        row.connect("notify::active", self._on_setting_changed, key)
        self._add_to(group, row)
        self.setting_widgets[key] = ("switch", row, None, 1)

    def _combo(self, group, key, title, values, labels):
        row = Adw.ComboRow(title=title, model=Gtk.StringList.new(labels))
        row.connect("notify::selected", self._on_setting_changed, key)
        self._add_to(group, row)
        self.setting_widgets[key] = ("combo", row, values, 1)

    def _widget_value(self, key):
        kind, row, extra, factor = self.setting_widgets[key]
        if kind == "spin":
            v = row.get_value() * factor
            return str(int(round(v))) if extra == 0 else f"{v:.{extra}f}"
        if kind == "switch":
            return "true" if row.get_active() else "false"
        return extra[row.get_selected()]

    # ------------------------------------------------ state sync

    def refresh(self):
        self.config = read_config()
        self._syncing = True
        try:
            disabled = common.get(self.config, "disabled") is True
            self.master_row.set_active(not disabled)
            self.confirm_row.set_active(common.get(self.config, "confirm.enabled") is True)
            self.confirm_row.set_sensitive(not disabled)
            for row, services, subtitle in self.pam_rows.values():
                states = [pam_enabled(s) for s in services]
                legacy = any(pam_has_module(old) for s in services for old in PAM_LEGACY.get(s, ())) or \
                    any(pam_has_module(s) and not pam_enabled(s) for s in services)
                row.set_active(all(states))
                if all(states):
                    pass
                elif legacy and not any(states):
                    subtitle = "Set up by an older version. Turn on to switch to the new method"
                elif any(states):
                    subtitle = "Only partly set up. Turn on to finish"
                row.set_subtitle(subtitle)
                row.set_sensitive(not disabled)

            for key, (kind, row, extra, factor) in self.setting_widgets.items():
                value = common.get(self.config, key)
                if kind == "spin":
                    try:
                        row.set_value(float(value) / factor)
                    except (TypeError, ValueError):
                        pass
                elif kind == "switch":
                    row.set_active(value is True)
                elif value in extra:
                    row.set_selected(extra.index(value))
            self.pending.clear()
            self.apply_banner.set_revealed(False)
        finally:
            self._syncing = False

        self._refresh_faces()
        self._refresh_cameras()
        self._update_banner()

    def _camera_path(self):
        """The configured camera if it exists, else None."""
        device = common.get(self.config, "camera", "")
        return device if isinstance(device, str) and device and os.path.exists(device) else None

    def _update_banner(self):
        if self._camera_path() is None:
            self.banner.set_title("No camera is configured")
            self.banner.set_button_label("Choose Camera")
            self.banner.set_revealed(True)
        else:
            self.banner.set_revealed(False)

    @staticmethod
    def _faces_description(encryption):
        text = f"Faces that can unlock the account “{USER}”. Stored as encrypted face templates, not photos."
        return text + (" " + ENCRYPTION_TEXT[encryption] if encryption in ENCRYPTION_TEXT else "")

    def _ask_engine(self, args, key, callback):
        """Run an engine command that answers with JSON and needs no password (as you), then
        call CALLBACK(output or None). Only the newest request per KEY is delivered."""
        self._engine_generation[key] = generation = self._engine_generation.get(key, 0) + 1
        cancel = Gio.Cancellable()
        GLib.timeout_add_seconds(15, lambda: cancel.cancel() and False)

        def finished(proc, res):
            try:
                _ok, out, _err = proc.communicate_utf8_finish(res)
            except GLib.Error:
                out = None
            if generation == self._engine_generation[key]:
                callback(out)

        try:
            proc = Gio.Subprocess.new([ENGINE, *args],
                                      Gio.SubprocessFlags.STDOUT_PIPE | Gio.SubprocessFlags.STDERR_SILENCE)
        except GLib.Error:
            callback(None)
            return
        proc.communicate_utf8_async(None, cancel, finished)

    def _refresh_faces(self):
        self._ask_engine(["list", "--username", USER], "list",
                         lambda out: self._show_faces(common.parse_face_list(out)))
        self._ask_engine(["status"], "status", self._show_encryption)

    def _show_encryption(self, out):
        # Without an answer from the daemon, work it out here from the hardware and the config
        self.encryption_status = common.parse_status(out, common.local_status(self.config))
        look = common.encryption_switch(self.encryption_status)
        self._syncing = True
        try:
            self.crypt_row.set_active(look["active"])
            self.crypt_row.set_sensitive(look["sensitive"])
            self.crypt_row.set_subtitle(look["subtitle"])
        finally:
            self._syncing = False

    def _show_faces(self, faces):
        for row in self.model_rows:
            self.models_group.remove(row)
        self.model_rows = []
        state = faces["state"]
        self.models_group.set_description(self._faces_description(faces["encryption"]))

        if state == "needs_reenrol":
            row = Adw.ActionRow(title="Your face data needs to be added again",
                                subtitle="It can't be used any more, for example because the recognition model "
                                         "changed or this PC's security chip was reset")
            btn = Gtk.Button(label="Add", valign=Gtk.Align.CENTER, css_classes=["suggested-action"])
            btn.connect("clicked", self._on_add_clicked)
            row.add_suffix(btn)
            self._add_model_row(row)
        elif state == "error":
            self._add_model_row(Adw.ActionRow(title="Face data could not be read",
                                              subtitle=GLib.markup_escape_text(faces["detail"])))
        elif state == "not_enrolled" or not faces["entries"]:
            self._add_model_row(Adw.ActionRow(title="No face models yet",
                                              subtitle="Add one to start using face unlock"))
        for number, entry in enumerate(faces["entries"], 1):
            when = time.strftime("%Y-%m-%d %H:%M", time.localtime(entry["created"])) \
                if entry["created"] else "at an unknown time"
            row = Adw.ActionRow(title=f"Face picture {number}", subtitle=f"Added {when}")
            row.add_prefix(Gtk.Image(icon_name="avatar-default-symbolic"))
            btn = Gtk.Button(icon_name="user-trash-symbolic", valign=Gtk.Align.CENTER,
                             tooltip_text="Remove", css_classes=["flat"])
            btn.connect("clicked", self._on_remove_clicked, entry["id"], number)
            row.add_suffix(btn)
            self._add_model_row(row)
        # Nothing to remove when nothing is enrolled
        self.clear_row.set_sensitive(state != "not_enrolled" and (state != "ok" or bool(faces["entries"])))

    def _add_model_row(self, row):
        self.models_group.add(row)
        self.model_rows.append(row)

    def _refresh_cameras(self):
        self.cameras = list_cameras()
        current = self._camera_path()
        current_real = os.path.realpath(current) if current else None

        self._syncing = True
        self.cam_model.splice(0, self.cam_model.get_n_items(), [c[1] for c in self.cameras])
        selected = next((i for i, c in enumerate(self.cameras)
                         if current_real and os.path.realpath(c[0]) == current_real), None)
        if selected is None:
            selected = next((i for i, c in enumerate(self.cameras) if c[3]), 0)
        if self.cameras:
            self.cam_row.set_selected(selected)
        self._syncing = False

        match = next((c for c in self.cameras if current_real and os.path.realpath(c[0]) == current_real), None)
        configured = common.get(self.config, "camera", "")
        self.cam_info.set_subtitle(match[1] if match else (configured or "Not set"))
        self._on_camera_selected()

    def _selected_camera(self):
        i = self.cam_row.get_selected()
        return self.cameras[i] if self.cameras and i < len(self.cameras) else None

    # ------------------------------------------------ privileged actions

    def run_helper(self, args, success=None, on_line=None, on_done=None, refresh=True):
        """Stop the camera, run the helper, then restore state."""
        if self.helper.busy:
            self.toast("Another action is still running")
            GLib.idle_add(self.refresh)
            return False
        was_previewing = self.preview_toggle.get_active()
        self.preview_toggle.set_active(False)

        def done(code, output):
            if code in (126, 127):
                self.toast("Authentication cancelled")
            elif code != 0 and on_done is None:
                last = output.strip().splitlines()[-1] if output.strip() else f"exit code {code}"
                self.toast(f"Failed: {last}")
            elif code == 0 and success:
                self.toast(success)
            if on_done:
                on_done(code, output)
            if refresh:
                self.refresh()
            if was_previewing and self.stack.get_visible_child_name() == "camera":
                self.preview_toggle.set_active(True)

        self.helper.run(args, on_line=on_line, on_done=done)
        return True

    def _on_master_toggled(self, row, _pspec):
        if self._syncing:
            return
        on = row.get_active()
        self.run_helper(["set", f"disabled={'false' if on else 'true'}"],
                        success="Face authentication " + ("enabled" if on else "disabled"))

    def _on_confirm_toggled(self, row, _pspec):
        if self._syncing:
            return
        on = row.get_active()
        self.run_helper(["set", f"confirm.enabled={'true' if on else 'false'}"],
                        success="Confirmation " + ("turned on" if on else "turned off"))

    def _on_crypt_toggled(self, row, _pspec):
        if self._syncing:
            return

        def done(code, output):
            if code in (126, 127):
                return  # run_helper already said the authentication was cancelled
            reply = next((l for l in reversed(output.splitlines()) if l.startswith("{")), "")
            try:
                reply = json.loads(reply)
            except ValueError:
                reply = {}
            if code == 0 and reply.get("result", "ok") == "ok":
                self.toast("Face data converted")
            else:
                detail = reply.get("detail") if isinstance(reply.get("detail"), str) else ""
                self.toast("Could not convert face data" + (f": {detail}" if detail else ""))

        # The engine converts the stored face data; a plain config change would leave it behind
        self.run_helper(["set-encryption", "on" if row.get_active() else "off"], on_done=done)

    def _on_pam_toggled(self, row, _pspec, services):
        if self._syncing:
            return
        if row.get_active():
            self.run_helper(["pam-enable", *services], success=f"Face unlock enabled for {row.get_title()}")
        else:
            self.run_helper(["pam-disable", *services], success=f"Face unlock disabled for {row.get_title()}")

    def _on_add_clicked(self, _btn):
        dialog = Adw.AlertDialog(heading="Add Your Face",
                                 body="Look straight into the camera. It takes 5 pictures one after "
                                      "another; turn your head a little between them.")
        dialog.add_response("cancel", "Cancel")
        dialog.add_response("add", "Start")
        dialog.set_response_appearance("add", Adw.ResponseAppearance.SUGGESTED)
        dialog.set_default_response("add")
        dialog.connect("response", lambda d, r: r == "add" and self._run_with_progress(
            "Adding Your Face", "Look straight into the camera…", ["enroll"],
            self._add_finished, self._add_progress))
        dialog.present(self)

    @staticmethod
    def _add_progress(line):
        """Text for a "PROGRESS 3 5" line from the engine, or None for other lines."""
        words = line.split()
        if len(words) == 3 and words[0] == "PROGRESS" and words[1].isdigit() and words[2].isdigit():
            return f"Picture {words[1]} of {words[2]}. Keep looking at the camera…"
        return None

    def _add_finished(self, code, output):
        lines = [l.strip() for l in output.splitlines() if l.strip()]
        done = next((l for l in reversed(lines) if l.startswith("OK")), None)
        if code == 0 and done:
            count = done.split()[1] if len(done.split()) > 1 and done.split()[1].isdigit() else "Your"
            return "Face Added", f"{count} pictures of your face were added. You can now test recognition."
        error = next((l for l in reversed(lines) if l.startswith("ERR")), None)
        reason = error[3:].strip() if error else (lines[-1] if lines else f"Error code {code}")
        known = TEST_RESULTS.get(reason)
        return "Could Not Add Face", known[1] if known else reason

    def _on_test_clicked(self, _btn):
        def finished(code, output):
            for line in reversed(output.strip().splitlines()):
                if line.startswith("{"):
                    try:
                        result = json.loads(line).get("result")
                    except ValueError:
                        break
                    if result in TEST_RESULTS:
                        return TEST_RESULTS[result]
                    break
            return "Test failed", f"Unexpected answer (exit code {code})\n{output.strip()}"
        self._run_with_progress("Testing Recognition", "Look at the camera…", ["test"], finished)

    def _run_with_progress(self, heading, body, args, describe, progress=None):
        spinner = Adw.Spinner(width_request=48, height_request=48)
        dialog = Adw.AlertDialog(heading=heading, body=body)
        dialog.set_extra_child(spinner)
        dialog.set_can_close(False)

        def on_line(line):
            text = progress(line) if progress else None
            if text:
                dialog.set_body(text)

        def on_done(code, output):
            dialog.set_can_close(True)
            if code in (126, 127):
                dialog.force_close()
                return
            title, text = describe(code, output)
            dialog.set_heading(title)
            dialog.set_body(text)
            icon = "emblem-ok-symbolic" if code == 0 else "dialog-warning-symbolic"
            dialog.set_extra_child(Gtk.Image(icon_name=icon, pixel_size=48,
                                             css_classes=["success" if code == 0 else "warning"]))
            dialog.add_response("close", "Close")

        if self.run_helper(args, on_line=on_line, on_done=on_done):
            dialog.present(self)

    def _on_remove_clicked(self, _btn, face_id, number):
        dialog = Adw.AlertDialog(heading="Remove Face Picture?",
                                 body=f"Face picture {number} will no longer be used to recognise you.")
        dialog.add_response("cancel", "Cancel")
        dialog.add_response("remove", "Remove")
        dialog.set_response_appearance("remove", Adw.ResponseAppearance.DESTRUCTIVE)
        dialog.connect("response", lambda d, r: r == "remove" and self.run_helper(
            ["remove-face", face_id], success="Face picture removed"))
        dialog.present(self)

    def _on_clear_clicked(self, _btn):
        dialog = Adw.AlertDialog(heading="Remove All Face Pictures?",
                                 body="Face unlock will stop working for this account until you add your face again.")
        dialog.add_response("cancel", "Cancel")
        dialog.add_response("clear", "Remove All")
        dialog.set_response_appearance("clear", Adw.ResponseAppearance.DESTRUCTIVE)
        dialog.connect("response", lambda d, r: r == "clear" and self.run_helper(
            ["clear-faces"], success="All face pictures removed"))
        dialog.present(self)

    def _on_use_camera(self, _btn):
        cam = self._selected_camera()
        if cam:
            self.run_helper(["set", f"camera={cam[0]}"], success=f"Now using {cam[1]}")

    def _on_setting_changed(self, _row, _pspec, key):
        if self._syncing:
            return
        self.pending[key] = self._widget_value(key)
        self.apply_banner.set_revealed(True)

    def _on_apply_settings(self, _banner):
        if self.pending:
            self.run_helper(["set"] + [f"{k}={v}" for k, v in self.pending.items()],
                            success="Settings saved")

    # ------------------------------------------------ camera page callbacks

    def _on_page_changed(self, *_):
        if self.stack.get_visible_child_name() != "camera":
            self.preview_toggle.set_active(False)

    def _on_camera_selected(self, *_):
        cam = self._selected_camera()
        current = self._camera_path()
        in_use = cam and current and os.path.realpath(cam[0]) == os.path.realpath(current)
        self.use_cam_btn.set_sensitive(bool(cam) and not in_use)
        self.cam_row.set_subtitle(cam[2] if cam else "No cameras found")
        if self._syncing:
            return
        if self.preview_toggle.get_active() and cam:
            self.preview.start(cam[0])

    def _on_preview_toggled(self, btn):
        cam = self._selected_camera()
        if btn.get_active() and cam and not self.helper.busy:
            self.preview.start(cam[0])
        else:
            if btn.get_active():
                btn.set_active(False)
                return
            self.preview.stop()
            self.dark_row.set_subtitle("—")
            self.strobe_row.set_subtitle("—")
            self.dark_bar.set_value(0)

    def _on_preview_stats(self, darkness, strobing, error):
        if error:
            self.preview_toggle.set_active(False)
            self.dark_row.set_subtitle(GLib.markup_escape_text(error))
            return
        self.dark_bar.set_value(darkness)
        self.dark_row.set_subtitle(f"≈{darkness:.0f}% dark (an estimate from lit pictures)")
        if strobing is None:
            self.strobe_row.set_subtitle("Checking…")
        elif strobing:
            self.strobe_row.set_subtitle("Yes. Lit and unlit pictures alternate, as the real-face check needs")
        else:
            self.strobe_row.set_subtitle("No. Turn off the IR light check in Settings if this camera has no flashing IR light")

    # ------------------------------------------------ misc

    def toast(self, text):
        self.toasts.add_toast(Adw.Toast(title=GLib.markup_escape_text(text), timeout=3))

    def _on_about(self, _btn):
        about = Adw.AboutDialog(
            application_name="Linux Hello Camera", application_icon="camera-web",
            version=VERSION, developer_name="Linux Hello Camera contributors",
            website="https://github.com/ducvd89/linux-hello-camera",
            comments="Face unlock for Linux with an IR camera, like Windows Hello.\n"
                     "The face engine is based on biopass (MIT).\n\n"
                     "Face recognition is more convenient than a password, not more secure. "
                     "Keep a password set on your account.",
            license_type=Gtk.License.MIT_X11)
        about.present(self)


class LinuxHelloCameraApp(Adw.Application):
    def __init__(self):
        super().__init__(application_id=APP_ID, flags=Gio.ApplicationFlags.DEFAULT_FLAGS)

    def do_activate(self):
        win = self.get_active_window() or LinuxHelloCameraWindow(self)
        win.present()


if __name__ == "__main__":
    sys.exit(LinuxHelloCameraApp().run(sys.argv))
