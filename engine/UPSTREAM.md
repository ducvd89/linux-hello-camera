# Upstream

This engine started from the `auth/` folder of [biopass](https://github.com/TickLabVN/biopass)
(MIT, Copyright (c) 2026 TickLab, see LICENSE.biopass), at commit 74af2fc
(release 1.4.1). What changed:

- Removed: fingerprint, libcamera, the SQLite model registry, the per-user config and data
  folders, `AuthManager` and its parallel/sequential modes, and the `capture-face`, `crop-face`
  and `preview-session` commands.
- Added: the root daemon `linux-hello-camerad` (socket-activated, SO_PEERCRED checks, encrypted
  embeddings-only face templates, rate limiting) with the helper as its client, and a V4L2 capture class for the IR camera, IR strobe liveness, `enroll`, `test`, `probe`
  and `probe-dir`, and the system-wide config and face store described in `DESIGN.md`.
- Kept: the YOLOv8n-face detector (plus its five landmarks, now used for the eye regions),
  the EdgeFace recognizer, the optional MiniFAS / MobileNetV3 anti-spoofing and the PAM module
  that runs the helper.
