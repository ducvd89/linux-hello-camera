# Linux Hello Camera: technical notes

For installing and using the app, see the [main README](../README.md). For what the engine and
the app promise each other, see [DESIGN.md](../DESIGN.md). This page covers what the installer
and the app change on your system.

## What `install.sh` does

1. Installs the build tools and runtime dependencies with pacman (`base-devel`, `git`, `cmake`,
   `ninja`, `v4l-utils`, `python-yaml`, GTK and libadwaita, ffmpeg, ...).
2. Builds the package from the root [`PKGBUILD`](../PKGBUILD) with `makepkg`, out of tree in
   `~/.cache/linux-hello-camera-install`. The engine's CMake build fetches ONNX Runtime and a few
   small libraries, and the five face models come from the biopass 1.4.1 release, so it needs
   internet.
3. Installs the package with `pacman -U`.
4. Finds the first IR camera (a `GREY`-format V4L2 device) and sets it as `camera` in the config,
   unless one is already set.

It doesn't turn face unlock on anywhere new. That's what the app's switches are for.

## Files

| Installed to | From | What |
|---|---|---|
| `/usr/bin/linux-hello-camera` | link to `linux_hello_camera.py` | the app |
| `/usr/lib/linux-hello-camera/linux_hello_camera.py` | `app/` | the GTK app, runs as you |
| `/usr/lib/linux-hello-camera/linux_hello_camera_common.py` | `app/` | config defaults and reading, socket and desktop helpers shared by the scripts below |
| `/usr/lib/linux-hello-camera/settings-helper` | `app/` | root helper, only started through pkexec |
| `/usr/lib/linux-hello-camera/input-gate` | `app/` | waits for a key press or click on the Plasma lock screen |
| `/usr/lib/linux-hello-camera/confirm-dialog` | `app/` | the confirmation window (runs as you) |
| `/usr/lib/linux-hello-camera/confirm-broker` | `app/` | opens the dialog before the face check (sudo, admin prompts) |
| `/usr/lib/linux-hello-camera/confirm-hook` | `app/` | run by the engine after a match, asks the dialog for the answer |
| `/usr/lib/linux-hello-camera/linux-hello-camera-helper` | `engine/` | the engine's command line (`auth`, `enroll`, `test`, `list`, `status`, ...), a thin client of the daemon |
| `/usr/lib/linux-hello-camera/linux-hello-camerad`, `/usr/lib/systemd/system/linux-hello-camerad.{socket,service}` | `engine/` | the daemon that opens the camera and the only process that can read face templates (see *Storage*) |
| `/usr/lib/security/pam_linux_hello_camera.so` | `engine/` | the PAM module |
| `/usr/share/linux-hello-camera/models/*.onnx` | biopass 1.4.1 | detection, recognition and anti-spoofing models |
| `/etc/linux-hello-camera/config.yaml` | `app/config.yaml` | settings (kept on upgrade) |
| `/var/lib/linux-hello-camera/templates/<user>.cred` or `<user>.json` | the daemon | face templates, root only (see *Storage*) |
| `/usr/share/polkit-1/actions/io.github.linux_hello_camera.policy` | `app/` | polkit action for the helper (`auth_admin_keep`: one password prompt, then cached) |
| `/usr/lib/systemd/system/polkit-agent-helper@.service.d/10-linux-hello-camera.conf` | `app/` | lets polkit's password helper open the camera |

The app reads the config and the PAM files directly, and asks the engine for the face list and the
encryption status (as you, no password needed). Anything that changes the
system goes through the helper, which accepts only a fixed list of commands and checks every
config key and value against a schema:

| Command | What it does |
|---|---|
| `enroll` | runs the engine's `enroll --username <you> --count 5` (always adds) and passes its `PROGRESS i N` lines on |
| `remove-face ID` | asks the engine to remove one of your entries; the id is digits only, as shown by `list` |
| `clear-faces` | asks the engine to remove all of your face data |
| `set-encryption on\|off\|auto` | asks the engine to lock face data to the TPM or not, and to convert what's stored |
| `test` | runs the engine's `test --username <you>`, prints its JSON answer |
| `set KEY=VALUE ...` | changes config options by dotted key, e.g. `recognition.threshold=0.5` |
| `pam-enable` / `pam-disable SERVICE...` | adds or removes the PAM lines |
| `pam-refresh` | rewrites every service that has face unlock with the current lines (upgrades) |

Face data always belongs to the account that ran pkexec. `set`, `pam-*` need no user, so the
installer and package hooks run them as plain root.

To run the app from the source folder without installing: `python app/linux_hello_camera.py`
(it needs the engine installed, otherwise it shows the "not installed" page).

## What the app's switches change

Each switch on the *Face Unlock* tab covers every installed service for that feature, and adds or
removes lines in each one's `auth` section, all ending in `# linux-hello-camera`:

```
auth       sufficient   pam_linux_hello_camera.so   # linux-hello-camera
```

| Switch | Files (whichever are installed) |
|---|---|
| sudo | `/etc/pam.d/sudo` |
| Login screen (*Login and lock screen* on GNOME) | `/etc/pam.d/gdm-password` (GDM), `/etc/pam.d/sddm` (SDDM), `/etc/pam.d/plasmalogin` (Plasma Login Manager) |
| Lock screen (KDE only) | `/etc/pam.d/kde-fingerprint`, plus one line in `/etc/pam.d/kde` (see below) |
| Administrator prompts | `/etc/pam.d/polkit-1` |

A switch shows as on only when every file in its row has the lines. If only some do (for example
after installing a second login manager), it says *Only partly set up*; turning it on finishes
the job. Files that only exist in `/usr/lib/pam.d` (`plasmalogin`, `kde-fingerprint`, `polkit-1`)
are copied to `/etc/pam.d` first, with a `# linux-hello-camera: copied from ...` comment, and
deleted again when you turn the switch off and the copy is unchanged.

The first change to a real file keeps the original as `<file>.linux-hello-camera.bak`. Only
lines with the marker are ever touched.

The master switch (*Face authentication*) sets `disabled: true` in the config. PAM lines stay,
but the engine skips face unlock everywhere.

**Why `kde-fingerprint` for the Plasma lock screen:** when the lock screen wakes up, Plasma starts
two things at once: the password prompt (`kde`) and a background check (`kde-fingerprint`, meant
for fingerprint readers). Putting face unlock in the background check means it starts without
pressing Enter and doesn't block typing your password. The line is placed after the nologin and
faillock checks and before `pam_fprintd.so`, so a fingerprint reader still works if your face
isn't recognised. If the face check fails and there's no fingerprint reader, Plasma won't retry
until the next time the screen locks.

**Waiting for a key press or click:** Plasma starts that background check on the first mouse
movement, even right after you lock the screen, so the camera would see you and unlock again.
So in `kde-fingerprint` the face check sits behind [`input-gate`](input-gate), which reads
`/dev/input` and only lets the scan start after a key press, mouse button or touchpad tap.
Mouse movement and fingers resting on the touchpad don't count, and input during the first
2 seconds after locking is ignored.

```
auth       requisite    pam_exec.so quiet quiet_log /usr/lib/linux-hello-camera/input-gate   # linux-hello-camera
auth       sufficient   pam_linux_hello_camera.so   # linux-hello-camera
```

If you unlock with your password while the gate is still waiting, a line at the end of `auth` in
`/etc/pam.d/kde` (`input-gate --release`) stops it, so the lock screen can close straight
away. The gate is `requisite`: if it fails or is missing, the face check fails and nothing
is skipped. It needs your account to be in the `input` group; without it, the gate lets the
scan start straight away and the app shows a note on the switch.

## Confirming sudo and admin prompts

*Confirm sudo and admin prompts* sets `confirm.enabled` in the config (default on, for the
services in `confirm.services`: `sudo` and `polkit-1`). After a face match, the engine runs
[`confirm-hook`](confirm-hook) as root, and approves only if it exits 0:

```
confirm-hook --user USER --service SERVICE --pam-pid PID
```

To get your attention before the face check, the sudo and admin-prompt switches also add two
lines around the face check:

```
auth       optional     pam_exec.so seteuid quiet quiet_log /usr/lib/linux-hello-camera/confirm-broker start   # linux-hello-camera
auth       sufficient   pam_linux_hello_camera.so   # linux-hello-camera
auth       optional     pam_exec.so seteuid quiet quiet_log /usr/lib/linux-hello-camera/confirm-broker failed   # linux-hello-camera
```

`start` opens [`confirm-dialog`](confirm-dialog) as you, in your desktop session, in its waiting
state (*Making sure it's you*, Continue greyed out, the command shown for sudo), and leaves a
small root process, [`confirm-broker`](confirm-broker), listening on a socket in
`/run/linux-hello-camera/confirm/` that only root can reach. It's named after the process doing the
authentication (sudo or polkit's password helper). After a face match, `confirm-hook` connects
there and sends `matched <seconds>`: Continue lights up and your answer (`yes` or `no`) goes back
to the hook. `failed` only runs when the face check didn't approve, and switches the dialog to
*Couldn't recognise you*. Both lines are `optional`, so they can't approve anything; if the
broker isn't there, the hook opens the dialog itself.

Only **Continue** approves. Cancel, Escape, closing the window, the timeout
(`confirm.timeout_s`, 15 s by default), having no desktop session, or any error makes the hook
exit 1, so the face check fails and the prompt falls back to your password. The hook never prints
anything (for admin prompts, stdout is polkit's protocol channel). Because the confirmation runs
inside the engine after the face match, it can't approve anything without your face.

## Liveness

A photo or a screen shows a face too, so the engine checks that what it sees is a real one. With
`ir_liveness.enabled` (default on), it uses the IR light of the camera, which flashes: every
other picture is lit. A real face gets clearly brighter in the lit picture than in the unlit
one, and brighter than the background does; a photo or a phone screen doesn't react like that.
`min_pairs`, `min_face_gain` and `min_gain_ratio` are the thresholds and are starting values; the
*Settings* tab has them under *Advanced*. The *Camera* tab's preview shows whether the camera
strobes, because without that the check can't pass.

`ai_antispoof` (off by default) runs two small neural networks on the picture. They are trained
on colour images, so they're for colour cameras, not for IR ones.

## How the app works

```
linux_hello_camera.py                  the app
linux_hello_camera_common.py           config defaults/reading, sockets, desktop session lookup
settings-helper                        root helper, only started through pkexec
input-gate                             waits for a key press or click on the Plasma lock screen
confirm-dialog, confirm-hook           confirmation dialog, and the engine's step that asks it
confirm-broker                         opens the dialog before the face check (sudo, admin prompts)
io.github.linux_hello_camera.policy    polkit action
io.github.linux_hello_camera.desktop
10-linux-hello-camera.conf             systemd drop-in for polkit's password helper
config.yaml                            the config the package ships
tests/                                 unit tests (python -m unittest discover -s app/tests)
../PKGBUILD, ../linux-hello-camera.install
```

The camera preview uses `ffmpeg` and `v4l2-ctl`. It shows the lit frames, a rough darkness value
and whether the IR light strobes (alternating bright and dark frames). It doesn't detect faces;
use *Test* for that.

## Storage and encryption

Face data is stored as **templates, never photos**: each enrolled picture becomes one entry, a
list of 512 numbers (an embedding) made by the recognition model. The picture itself is thrown
away right after, and the numbers can't be turned back into a picture of your face.

- **Where:** `/var/lib/linux-hello-camera/templates/<user>.tpm-sb.cred` (TPM and Secure Boot),
  `<user>.tpm.cred` (TPM only) or `<user>.json` (not encrypted), in a root-only folder. Only one
  of them exists per user.
- **Who reads it:** only `linux-hello-camerad`, a root daemon that systemd starts when something
  connects to `/run/linux-hello-camera/engine.sock` and stops after 60 idle seconds. The PAM
  module, the app and the root helper all talk to it through `linux-hello-camera-helper`. The
  daemon also opens the camera, so the polkit password helper's sandbox no longer needs device
  access. The only thing its drop-in still allows is `/run/linux-hello-camera`, where
  `confirm-broker` creates its socket (the helper mounts `/run` read-only otherwise).
- **Encryption:** `systemd-creds` with the TPM, so the file can only be opened on this PC. When
  Secure Boot is on, the key is also bound to it (PCR 7). The user name is part of what's
  encrypted, so a template can't be moved to another user.
- **The switch:** *Lock face data to this PC's security chip* on the *Face Unlock* tab sets
  `storage.tpm_encryption` (`auto`, `on` or `off`) through `settings-helper set-encryption`, and
  the daemon converts the stored templates in place, so nobody has to add their face again.

| This PC | Default (`auto`) | The switch |
|---|---|---|
| TPM 2.0 and Secure Boot on | encrypted: TPM and Secure Boot | available, on |
| TPM 2.0, Secure Boot off | not encrypted | available, off; the subtitle says Secure Boot is off. Turned on, it's the TPM only |
| No TPM 2.0 | not encrypted | greyed out, off; says so |

The status the app shows is `tpm-sb`, `tpm` or `none`, from the daemon's `status` and `list`.
What the Secure Boot binding adds: someone who boots your laptop from a USB stick, or from
anything else that changes the Secure Boot state, can't get the TPM to decrypt the templates, even
with root there. What it costs: changing the Secure Boot keys (or turning it off) makes the
templates unreadable, so the app shows *Your face data needs to be added again* and you add your
face again; the password works meanwhile.

The app asks the daemon (`linux-hello-camera-helper status`) and, if it can't be reached, works
out the same answer itself from `/dev/tpmrm0`, `/sys/class/tpm/tpm0/tpm_version_major` and the
`SecureBoot` EFI variable, by the rules in [DESIGN.md](../DESIGN.md).

A template that can't be opened (the TPM was cleared, or the recognition model changed) is
shown as *Your face data needs to be added again*, and until then face unlock is skipped and the
password works.

**What this protects, and what it doesn't.** Someone who copies the templates (a backup, a
stolen disk image, another user) gets nothing they can use or turn into a photo, as long as
they're encrypted and the TPM isn't theirs. It doesn't stop root on the running system: root can
ask the daemon to run face checks, enrol faces or remove them, and can turn encryption off.
Without a TPM, or with the switch off, the file is a plain list of numbers. For a stolen laptop
overall, full-disk encryption is still the answer.

## Upgrading from 0.8

Version 0.8 kept cropped pictures of your face in `/var/lib/linux-hello-camera/faces/<you>/`.
On install or upgrade the package enables the daemon and runs `linux-hello-camera-helper migrate`:
it makes templates from the pictures with the configured model (encrypted if the settings say so),
overwrites the pictures with zeros and deletes them and the folder. A one-line summary is
printed; if it fails, add your face again in the app.

## Tests

```
python -m unittest discover -s app/tests
```

They cover the helper's PAM editing (on temporary copies of realistic files), the config schema,
the face list and entry ids, the TPM switch logic (with fake sysfs and efivars files), the package scripts' syntax, the confirmation hook and the input
gate's trigger logic. They never touch `/etc`.

## Tested hardware

ASUS laptop, CachyOS, GNOME 50, with a Shinetech "USB2.0 FHD UVC WebCam". The IR sensor is
`/dev/video2` (640×400 GREY, 15 fps). The IR stream stops after about 8 s, so the engine and the
preview reopen the stream on their own.
