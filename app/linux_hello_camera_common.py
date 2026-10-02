"""Shared by the app, the root helper, confirm-hook and confirm-broker.

The config part runs in all of them: the GTK app reads it as the user, the others as root. The
confirmation helpers (hook and broker) run as root and must never get in the way of
authentication, so everything here raises ordinary exceptions and they decide what to do.
"""

import copy
import json
import os
import re
import socket
import stat
import subprocess

try:
    import yaml
except ImportError:  # python-yaml is a package dependency; callers show a clear message
    yaml = None

LIB_DIR = "/usr/lib/linux-hello-camera"
CONFIG_PATH = "/etc/linux-hello-camera/config.yaml"
TEMPLATES_DIR = "/var/lib/linux-hello-camera/templates"
ENGINE = LIB_DIR + "/linux-hello-camera-helper"
DIALOG = LIB_DIR + "/confirm-dialog"
# The engine daemon's own socket (engine.sock) lives in /run/linux-hello-camera, which has to be
# traversable by everyone. The broker's sockets get a private root-only folder below it.
SOCKET_DIR = "/run/linux-hello-camera/confirm"
# Variables taken from the user's systemd --user environment for the dialog
SESSION_VARS = ("WAYLAND_DISPLAY", "DISPLAY", "XAUTHORITY", "XDG_CURRENT_DESKTOP",
                "XDG_SESSION_DESKTOP", "XDG_SESSION_TYPE", "LANG", "LANGUAGE", "LC_ALL",
                "LC_MESSAGES")

# What the engine assumes for every key that is missing from the file (see DESIGN.md). The
# camera is empty until the installer or the app picks one.
DEFAULTS = {
    "schema_version": 1,
    "disabled": False,
    "camera": "",
    "timeout_ms": 4000,
    "abort_if_lid_closed": True,
    "abort_if_ssh": True,
    "ignore_services": [],
    "detection": {
        "model": "yolov8n-face.onnx",
        "threshold": 0.5,
    },
    "recognition": {
        "model": "edgeface_s_gamma_05.onnx",
        "threshold": 0.5,
        "frames_needed": 2,
    },
    "ir_liveness": {
        "enabled": True,
        "min_pairs": 2,
        "min_face_gain": 25.0,
        "min_gain_ratio": 1.8,
    },
    "storage": {
        "tpm_encryption": "auto",
    },
    "ai_antispoof": {
        "enabled": False,
        "model": "minifas_v2.onnx",
        "threshold": 0.8,
    },
    "confirm": {
        "enabled": True,
        "services": ["sudo", "polkit-1"],
        "timeout_s": 15,
    },
}


class ConfigError(Exception):
    """The config can't be used: python-yaml is missing or the file isn't valid YAML."""


def need_yaml():
    if yaml is None:
        raise ConfigError("python-yaml is not installed (sudo pacman -S python-yaml)")


def merge(base, extra):
    """BASE with the values of EXTRA laid over it. Nested sections merge; unknown keys stay."""
    result = copy.deepcopy(base)
    for key, value in extra.items():
        if isinstance(value, dict) and isinstance(result.get(key), dict):
            result[key] = merge(result[key], value)
        else:
            result[key] = copy.deepcopy(value)
    return result


def read_raw(path=None):
    """The file as written, without defaults. A missing or empty file is {}."""
    need_yaml()
    try:
        with open(path or CONFIG_PATH) as f:
            data = yaml.safe_load(f)
    except FileNotFoundError:
        return {}
    except (OSError, yaml.YAMLError) as e:
        raise ConfigError(f"Could not read the config: {e}") from e
    if data is None:
        return {}
    if not isinstance(data, dict):
        raise ConfigError("The config is not a YAML mapping")
    return data


def load_config(path=None):
    """The config with every missing key set to its default."""
    return merge(DEFAULTS, read_raw(path))


def get(config, dotted, default=None):
    """config["a"]["b"] for "a.b", or DEFAULT when a level is missing."""
    node = config
    for part in dotted.split("."):
        if not isinstance(node, dict) or part not in node:
            return default
        node = node[part]
    return node


def confirm_enabled(service):
    """Whether a match for SERVICE has to be confirmed (and so the early dialog is wanted)."""
    cfg = load_config()
    if get(cfg, "disabled") is True or service in (get(cfg, "ignore_services") or []):
        return False
    return get(cfg, "confirm.enabled") is True and service in (get(cfg, "confirm.services") or [])


TEMPLATE_EXTENSIONS = (".json", ".tpm.cred", ".tpm-sb.cred", ".cred")
ENCRYPTION_MODES = ("tpm-sb", "tpm", "none")


def has_faces(username):
    """Whether USERNAME has enrolled face templates. The engine ignores users who haven't.
    Only looks for the file: the templates are encrypted and only the daemon can open them."""
    if not username or "/" in username or username.startswith("."):
        return False
    # One file per user: .json (not encrypted), .tpm.cred (TPM) or .tpm-sb.cred (TPM and Secure
    # Boot). A plain .cred is accepted too, for safety.
    return any(os.path.exists(os.path.join(TEMPLATES_DIR, username + ext)) for ext in TEMPLATE_EXTENSIONS)


def parse_face_list(text):
    """Turn the output of `linux-hello-camera-helper list` into a dict for the app:
    {"state": "ok", "entries": [{"id", "created"}], "encryption": ..., "detail": ...}
    where state is ok, not_enrolled, needs_reenrol or error."""
    reply = None
    for line in reversed((text or "").splitlines()):
        if line.strip().startswith("{"):
            try:
                reply = json.loads(line)
            except ValueError:
                pass
            break
    if not isinstance(reply, dict):
        return {"state": "error", "entries": [], "encryption": None, "detail": "No answer from the engine"}
    result = reply.get("result")
    detail = reply.get("detail") if isinstance(reply.get("detail"), str) else ""
    if result == "ok":
        entries = []
        for entry in reply.get("entries") or []:
            if isinstance(entry, dict) and isinstance(entry.get("id"), str) and re.fullmatch(r"[0-9]{1,20}", entry["id"]):
                created = entry.get("created")
                entries.append({"id": entry["id"],
                                "created": created if isinstance(created, (int, float)) and not isinstance(created, bool) else None})
        encryption = reply.get("encryption") if reply.get("encryption") in ENCRYPTION_MODES else None
        return {"state": "ok", "entries": entries, "encryption": encryption, "detail": ""}
    if result in ("not_enrolled", "needs_reenrol"):
        return {"state": result, "entries": [], "encryption": None, "detail": detail}
    return {"state": "error", "entries": [], "encryption": None, "detail": detail or "Unexpected answer from the engine"}


# ---------------------------------------------------------------- TPM encryption of face data

TPM_SETTINGS = ("auto", "on", "off")
SECURE_BOOT_VAR = "/sys/firmware/efi/efivars/SecureBoot-8be4df61-93ca-11d2-aa0d-e98c2b8c"


def detect_hardware(root="/"):
    """(has TPM 2.0, Secure Boot on) by the same rules as the engine (see DESIGN.md). ROOT is
    only for tests: the paths below are looked up under it."""
    def path(p):
        return os.path.join(root, p.lstrip("/"))

    tpm2 = False
    try:
        with open(path("/sys/class/tpm/tpm0/tpm_version_major")) as f:
            tpm2 = os.path.exists(path("/dev/tpmrm0")) and f.read().strip() == "2"
    except OSError:
        pass
    secure_boot = False
    try:
        # 4 bytes of attributes, then the value
        with open(path(SECURE_BOOT_VAR), "rb") as f:
            data = f.read(5)
        secure_boot = len(data) == 5 and data[4] == 1
    except OSError:
        pass
    return tpm2, secure_boot


def effective_encryption(setting, tpm2, secure_boot):
    """"tpm-sb", "tpm" or "none": what the engine does for SETTING on this hardware. With a
    TPM and encryption on, the key is also bound to Secure Boot when that is on."""
    if not tpm2 or setting == "off":
        return "none"
    if setting == "auto" and not secure_boot:
        return "none"
    return "tpm-sb" if secure_boot else "tpm"


def local_status(config, root="/"):
    """What `linux-hello-camera-helper status` would say, worked out here. Used when the daemon
    can't be reached, so the switch still shows the right state."""
    tpm2, secure_boot = detect_hardware(root)
    setting = get(config, "storage.tpm_encryption")
    if setting not in TPM_SETTINGS:
        setting = "auto"
    return {"tpm2": tpm2, "secure_boot": secure_boot, "setting": setting,
            "effective": effective_encryption(setting, tpm2, secure_boot)}


def parse_status(text, fallback=None):
    """The engine's `status` answer as a dict, or FALLBACK when there isn't a usable one."""
    for line in reversed((text or "").splitlines()):
        if line.strip().startswith("{"):
            try:
                reply = json.loads(line)
            except ValueError:
                break
            if (isinstance(reply, dict) and isinstance(reply.get("tpm2"), bool)
                    and isinstance(reply.get("secure_boot"), bool) and reply.get("setting") in TPM_SETTINGS
                    and reply.get("effective") in ENCRYPTION_MODES):
                return {k: reply[k] for k in ("tpm2", "secure_boot", "setting", "effective")}
            break
    return fallback


def encryption_switch(status):
    """How the app's "lock face data to the security chip" switch looks for STATUS:
    {"sensitive": bool, "active": bool, "subtitle": str}."""
    if not status["tpm2"]:
        return {"sensitive": False, "active": False,
                "subtitle": "This PC has no TPM 2.0 security chip, so face data is stored without "
                            "encryption (as templates, never photos)"}
    active = status["effective"] in ("tpm-sb", "tpm")
    if not status["secure_boot"]:
        note = "Secure Boot is off, so this is off by default. You can still turn it on."
        if active:
            note = "Encrypted with the TPM only: Secure Boot is off on this PC."
        return {"sensitive": True, "active": active, "subtitle": note}
    if active:
        note = ("Encrypted with the TPM and tied to Secure Boot. Changing Secure Boot keys means "
                "adding your face again.")
    else:
        note = "Turn on to encrypt face data so it can only be opened on this PC"
    return {"sensitive": True, "active": active, "subtitle": note}


def confirm_timeout():
    """Seconds the dialog waits for Continue after a match."""
    value = get(load_config(), "confirm.timeout_s")
    return value if isinstance(value, int) and not isinstance(value, bool) and 1 <= value <= 600 else 15


def as_user(user):
    """subprocess arguments that run a command as USER (a pwd entry)."""
    return {"user": user.pw_uid, "group": user.pw_gid,
            "extra_groups": os.getgrouplist(user.pw_name, user.pw_gid)}


def desktop_env(user):
    """Environment for running a GUI app in USER's desktop session, or None if there's none."""
    runtime = f"/run/user/{user.pw_uid}"
    env = {"HOME": user.pw_dir, "USER": user.pw_name, "LOGNAME": user.pw_name,
           "PATH": "/usr/bin:/bin", "XDG_RUNTIME_DIR": runtime,
           "DBUS_SESSION_BUS_ADDRESS": f"unix:path={runtime}/bus"}
    shown = subprocess.run(["/usr/bin/systemctl", "--user", "show-environment"], env=env,
                           stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                           stderr=subprocess.DEVNULL, text=True, timeout=5, **as_user(user)).stdout
    for line in shown.splitlines():
        key, sep, value = line.partition("=")
        if sep and key in SESSION_VARS and not value.startswith("$'"):
            env[key] = value
    if "WAYLAND_DISPLAY" not in env and "DISPLAY" not in env:
        return None
    # The dialog may start inside polkit's sandbox: no GPU and no writable+executable memory, so
    # use GTK's software renderer, and keep GTK settings in memory (/run/user is read-only there)
    env.update(GSK_RENDERER="cairo", GSETTINGS_BACKEND="memory")
    return env


def sudo_command(pid):
    with open(f"/proc/{pid}/cmdline", "rb") as f:
        return " ".join(a.decode(errors="replace") for a in f.read().split(b"\0") if a)[:300]


def socket_path(pam_pid):
    """Where the broker for the authentication running in process PAM_PID listens."""
    return f"{SOCKET_DIR}/{pam_pid}.sock"


def trusted_socket(pam_pid):
    """The broker's socket path if it exists and only root could have made it, else None."""
    path = socket_path(pam_pid)
    try:
        folder, sock = os.lstat(SOCKET_DIR), os.lstat(path)
    except OSError:
        return None
    if folder.st_uid != 0 or not stat.S_ISDIR(folder.st_mode) or folder.st_mode & 0o077:
        return None
    if sock.st_uid != 0 or not stat.S_ISSOCK(sock.st_mode):
        return None
    return path


def listen(pam_pid):
    os.makedirs(SOCKET_DIR, mode=0o700, exist_ok=True)
    folder = os.lstat(SOCKET_DIR)
    if folder.st_uid != 0 or not stat.S_ISDIR(folder.st_mode) or folder.st_mode & 0o077:
        raise PermissionError(f"{SOCKET_DIR} is not a private root folder")
    path = socket_path(pam_pid)
    try:
        os.unlink(path)
    except FileNotFoundError:
        pass
    server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    server.bind(path)
    os.chmod(path, 0o600)
    server.listen(4)
    return server
