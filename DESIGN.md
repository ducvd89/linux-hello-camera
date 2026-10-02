# Linux Hello Camera: design

Face unlock for Linux with an IR camera, like Windows Hello. The face engine started from
[biopass](https://github.com/TickLabVN/biopass) (see `engine/UPSTREAM.md`); the desktop app is
a GTK4/libadwaita app. This file is the contract between the engine (`engine/`,
C++) and the app (`app/`, Python/GTK). Keep it up to date when either side changes.

## Layout

```
engine/   C++ face engine: PAM module, helper (client) and daemon (CMake)
app/      GTK4/libadwaita app, root helper (pkexec), input gate, confirm dialog + broker, PKGBUILD
install.sh
```

## Installed files

| Path | What |
|---|---|
| `/usr/lib/security/pam_linux_hello_camera.so` | PAM module (runs the helper, maps its exit code) |
| `/usr/lib/linux-hello-camera/linux-hello-camera-helper` | Engine CLI (below); a thin client of the daemon for everything that touches face data |
| `/usr/lib/linux-hello-camera/linux-hello-camerad` | Engine daemon, root, socket-activated: the only process that opens the camera for auth/enroll/test and the only one that can decrypt face templates |
| `/usr/lib/systemd/system/linux-hello-camerad.{socket,service}` | `ListenStream=/run/linux-hello-camera/engine.sock`, `SocketMode=0666` (access is checked per request with SO_PEERCRED); the service exits after 60 s idle |
| `/usr/lib/linux-hello-camera/libonnxruntime.so*` | Bundled ONNX Runtime (helper RUNPATH points here) |
| `/usr/share/linux-hello-camera/models/*.onnx` | Models from biopass release 1.4.1 |
| `/etc/linux-hello-camera/config.yaml` | System config, root:root 0644 (app writes it via its root helper) |
| `/var/lib/linux-hello-camera/templates/<user>.{json,tpm.cred,tpm-sb.cred}` | The user's face templates: **embeddings only, no images**. With TPM encryption on: encrypted with `systemd-creds encrypt --with-key=host+tpm2 --name=linux-hello-camera-<user>` (the name ties a file to its user, so files can't be swapped), as `<user>.tpm-sb.cred` with `--tpm2-pcrs=7` when Secure Boot is on (only decrypts while the PC boots with the same Secure Boot state and keys), or `<user>.tpm.cred` without PCR binding when Secure Boot is off. With it off: `<user>.json`, plain embeddings (option 1). Directory root:root 0700, files 0600 either way. Only one exists per user. |
| `/usr/lib/linux-hello-camera/confirm-hook` | Confirmation step for sudo / admin prompts (app side) |
| `/usr/lib/linux-hello-camera/input-gate` | Key/click gate for the Plasma lock screen (app side) |
| `/usr/lib/linux-hello-camera/confirm-broker`, `confirm-dialog` | Early "Making sure it's you" dialog (app side) |
| `/usr/lib/linux-hello-camera/settings-helper` | The app's pkexec root helper |

Models: `yolov8n-face.onnx` (detection), `edgeface_s_gamma_05.onnx` (default recognition),
`edgeface_xs_gamma_06.onnx` (faster recognition), `minifas_v2.onnx` and
`mobilenetv3_antispoof.onnx` (optional AI anti-spoofing, trained on RGB, off by default). Source:
`https://github.com/ducvd89/linux-hello-camera/releases/download/models-v1/<name>` (unchanged copies of the
models published with biopass 1.4.1; sources and licences in the release notes); sha256 pinned in the PKGBUILD.

## config.yaml

```yaml
schema_version: 1
disabled: false                 # master switch: auth returns "ignore" when true
camera: /dev/v4l/by-path/...    # the IR camera (GREY preferred; YUYV/MJPEG also accepted)
timeout_ms: 4000                # give up after this long without a match
abort_if_lid_closed: true
abort_if_ssh: true              # skip face unlock for remote (SSH) sessions; PAM falls back to the password
ignore_services: []             # PAM services where auth returns "ignore"
detection:
  model: yolov8n-face.onnx
  threshold: 0.5
recognition:
  model: edgeface_s_gamma_05.onnx
  threshold: 0.5                # cosine similarity needed for a frame to match
  frames_needed: 2              # matching lit frames needed (out of the last 3)
ir_liveness:                    # real face vs photo/screen, using the IR emitter's strobe
  enabled: true
  min_pairs: 2                  # lit/unlit pairs that must pass
  min_face_gain: 25.0           # mean(face lit) - mean(face unlit), grey levels
  min_gain_ratio: 1.8           # face gain / background gain
storage:
  tpm_encryption: auto          # auto | on | off. auto = on when the PC has a TPM 2.0 *and*
                                # Secure Boot is enabled, otherwise off. "on" without a TPM 2.0
                                # behaves like off (and the app greys the toggle out).
ai_antispoof:
  enabled: false
  model: minifas_v2.onnx
  threshold: 0.8
confirm:
  enabled: true                 # ask before approving these services after a match
  services: [sudo, polkit-1]
  timeout_s: 15
```

Unknown keys are ignored; missing keys take the defaults above. Thresholds for `ir_liveness` are
starting values, to be calibrated on real recordings (see `probe`).

## Face templates

Decrypted content (JSON): `{"version": 1, "model": "<recognition model file>", "dim": 512,
"entries": [{"id": "<unix-ms>", "created": <unix-s>, "embedding": "<base64 little-endian float32>"}]}`.
Each enrolled picture is one entry. A template made with a different recognition model than the
configured one, or one that can't be decrypted (TPM cleared, host secret changed), counts as
**needs re-enrolment**: auth returns ignore (password works), `list` reports it, the app asks the
user to add their face again. Embeddings can't be turned back into a photo of the face.

Upgrade from 0.8 (PNG crops in `/var/lib/linux-hello-camera/faces/<user>/`): the daemon command
`migrate` embeds every crop with the configured model, writes the encrypted template, then deletes
the crops (overwrite with zeros, unlink) and the faces directory. The package's post_upgrade runs it.

Hardware detection (engine and app use the same rules): TPM 2.0 = `/dev/tpmrm0` exists and
`/sys/class/tpm/tpm0/tpm_version_major` is `2`. Secure Boot = the 5th byte of
`/sys/firmware/efi/efivars/SecureBoot-8be4df61-93ca-11d2-aa0d-00e098032b8c` is 1 (missing file = off).
The wanted file kind is `json` (effective none), `tpm` (tpm2, Secure Boot off) or `tpm-sb` (tpm2,
Secure Boot on). Changing `storage.tpm_encryption` (app toggle → settings-helper → helper `set-encryption on|off`
→ daemon) converts every user's template to the wanted kind in place (write the new file atomically, then
remove the old one), so nobody has to re-enrol. On startup and before using a user's template, if
the file kind doesn't match the wanted kind (config edited by hand, Secure Boot switched on or
off), the daemon converts it the same way. A `.cred` file it can't decrypt (e.g. a `tpm-sb` file
after Secure Boot keys changed) counts as needs re-enrolment; the password still works.

## Daemon protocol

Unix stream socket `/run/linux-hello-camera/engine.sock`, one request per connection,
newline-delimited JSON. The daemon reads the peer's uid/pid with SO_PEERCRED.

| Request | Who may ask | Replies |
|---|---|---|
| `{"cmd":"auth","username":U,"service":S,"remote":bool}` | root, or the uid of U itself (the KDE lock screen asks as the user) | `{"result": ...}` with the same values as `test`, plus `"ignore"` |
| `{"cmd":"test","username":U}` | root | `{"result":..., "best_score":..., "liveness_pairs":..., "elapsed_ms":..., ...}` |
| `{"cmd":"enroll","username":U,"count":5}` | root | `{"progress":i,"total":N}` lines, then `{"result":"ok","added":n}` or `{"result":"error","detail":...}`; always appends |
| `{"cmd":"list","username":U}` | root, or the uid of U | `{"result":"ok","entries":[{"id","created"}],"encryption":"tpm-sb"\|"tpm"\|"none","model":...}` or `{"result":"not_enrolled"}` / `{"result":"needs_reenrol","detail":...}` (never embeddings) |
| `{"cmd":"remove","username":U,"id":ID}` / `{"cmd":"clear","username":U}` | root | `{"result":"ok"}` |
| `{"cmd":"migrate"}` | root | `{"result":"ok","migrated":[users],"failed":[{"username","detail"}]}`; crops of a user that can't be embedded are kept and listed in `failed` |
| `{"cmd":"set-encryption","value":"on"\|"off"\|"auto"}` | root | writes `storage.tpm_encryption`, converts all templates, `{"result":"ok","effective":"tpm-sb"\|"tpm"\|"none","converted":n}`; `"on"` without a TPM 2.0 → `{"result":"error","detail":"no TPM 2.0"}` |
| `{"cmd":"status"}` | anyone | `{"tpm2":bool,"secure_boot":bool,"setting":"auto"\|"on"\|"off","effective":"tpm-sb"\|"tpm"\|"none"}` |

Rate limit: after 5 failed face attempts for a user within 5 minutes, `auth` returns ignore for
2 minutes (password still works). Failures are counted per user in memory; the daemon stays up
while it has recent failures. Only one camera session at a time: a second request waits up to
5 s, then gets `{"result":"busy"}` (auth treats busy as ignore).

## Helper CLI (`linux-hello-camera-helper`)

`auth` is what PAM runs. **It must never write to stdout or stderr**: for admin prompts those are
polkit's protocol channel. It logs to syslog (`LOG_AUTHPRIV`, ident `linux-hello-camera`).

| Command | Run by | Output | Exit |
|---|---|---|---|
| `auth --username U [--service S]` | PAM module (as root for sudo/polkit/SDDM, as the user for the KDE lock screen) | nothing | 0 match, 1 no match, 2 ignore (disabled, not enrolled, ignored service, lid closed, remote (SSH) session, no camera) |
| `test --username U` | app's root helper | one JSON line: `{"result": "ok"\|"not_recognised"\|"no_face"\|"liveness_failed"\|"not_enrolled"\|"needs_reenrol"\|"camera_unavailable"\|"too_dark"\|"busy"\|"error", "best_score": f, "liveness_pairs": n, "elapsed_ms": n}` | 0 if ok else 1 |
| `enroll --username U [--count 5]` | app's root helper (root) | progress lines `PROGRESS i N`, then `OK n` or `ERR <reason>`. Adds to the user's existing template | 0 ok |
| `list --username U` | the app (as the user) or its root helper | the daemon's `list` reply as one JSON line | 0 |
| `remove --username U --id ID`, `clear --username U`, `migrate`, `set-encryption on\|off\|auto` | app's root helper / package scripts (root) | the daemon's reply as one JSON line | 0 ok |
| `status` | the app (as the user) | the daemon's `status` reply as one JSON line | 0 |
| `probe [--seconds 5] [--camera DEV]` | developer / calibration | JSON lines per lit/unlit pair: face box, detection score, face gain, background gain, ratio | 0 |
| `probe-dir DIR` | developer / tests | same as `probe`, reading recorded PNG frames + `session.json` (`frames: [{file, lit, mean}]`) | 0 |

`auth`, `test`, `enroll`, `list`, `remove`, `clear` and `migrate` send the request to the daemon
(socket activation starts it). The helper keeps the checks that need the PAM caller's context
(remote session, ignored service, lid) and the confirm hook; the daemon does the rest. If the
daemon can't be reached, `auth` returns ignore.

Face check flow (in the daemon): read config; return ignore for ignore cases; open the camera (V4L2, mmap); read frames
until `timeout_ms`; skip uniform frames (min == max: this camera stops streaming after ~8 s,
reopen); classify lit/unlit by mean brightness; on lit frames detect the largest face, embed it,
compare with the user's decrypted template (cosine); pair each lit frame with the neighbouring unlit
frame for liveness; succeed when `frames_needed` of the last 3 lit frames match and
`ir_liveness.min_pairs` pairs pass (and AI anti-spoof passes if enabled). Then, if
`confirm.enabled` and the service is in `confirm.services`, run
`/usr/lib/linux-hello-camera/confirm-hook --user U --service S --pam-pid <helper's parent pid>`
and succeed only if it exits 0.

`/run/linux-hello-camera/` holds `engine.sock` and must be reachable by every user (the KDE lock
screen asks as the user). The confirm broker's sockets live in `/run/linux-hello-camera/confirm/`
(root, 0700). The polkit-agent-helper drop-in only adds `ReadWritePaths=-/run/linux-hello-camera`
so the broker can create its socket inside polkit's sandbox; the agent no longer needs camera access.

## PAM lines (written by the app's switches, marked `# linux-hello-camera`)

```
# sudo, polkit-1
auth optional   pam_exec.so seteuid quiet quiet_log /usr/lib/linux-hello-camera/confirm-broker start
auth sufficient pam_linux_hello_camera.so
auth optional   pam_exec.so seteuid quiet quiet_log /usr/lib/linux-hello-camera/confirm-broker failed
# kde-fingerprint (Plasma lock screen), plus a --release line at the end of auth in kde
auth requisite  pam_exec.so quiet quiet_log /usr/lib/linux-hello-camera/input-gate
auth sufficient pam_linux_hello_camera.so
# gdm-password, sddm, plasmalogin
auth sufficient pam_linux_hello_camera.so
```
