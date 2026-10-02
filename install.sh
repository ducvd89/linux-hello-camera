#!/usr/bin/env bash
# Install Linux Hello Camera (face unlock with an IR camera) on Arch-based distros
# (CachyOS, Arch, EndeavourOS, ...). Run as your normal user, not root.
#
#   ./install.sh               build and install it, and pick your IR camera
#   ./install.sh --uninstall   remove the PAM lines and the package again
#   ./install.sh --yes         don't ask questions
set -euo pipefail

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WORK_DIR="${XDG_CACHE_HOME:-$HOME/.cache}/linux-hello-camera-install"
CONFIG=/etc/linux-hello-camera/config.yaml
HELPER=/usr/lib/linux-hello-camera/settings-helper
SERVICES=(sudo polkit-1 gdm-password sddm plasmalogin kde-fingerprint kde)

UNINSTALL=0
ASSUME_YES=0

bold() { printf '\n\033[1m==> %s\033[0m\n' "$*"; }
info() { printf '    %s\n' "$*"; }
warn() { printf '\033[33m    warning: %s\033[0m\n' "$*" >&2; }
die()  { printf '\033[31merror: %s\033[0m\n' "$*" >&2; exit 1; }

usage() {
	sed -n '2,7p' "$0" | sed 's/^# \{0,1\}//'
	cat <<-EOF

	Options:
	  --uninstall   disable face unlock in PAM and remove the package
	  -y, --yes     don't ask for confirmation
	  -h, --help    show this help
	EOF
}

while [[ $# -gt 0 ]]; do
	case "$1" in
		--uninstall) UNINSTALL=1 ;;
		-y|--yes) ASSUME_YES=1 ;;
		-h|--help) usage; exit 0 ;;
		*) usage; die "unknown option: $1" ;;
	esac
	shift
done

# Questions that default to yes
confirm() {
	[[ $ASSUME_YES -eq 1 ]] && return 0
	read -r -p "    $1 [Y/n] " reply
	[[ -z "$reply" || "$reply" =~ ^[Yy] ]]
}

installed() { pacman -Qq "$1" >/dev/null 2>&1; }

# Print the stable /dev/v4l/by-path link of the first IR (greyscale) camera
find_ir_camera() {
	local dev link
	for dev in /dev/video*; do
		[[ -e "$dev" ]] || continue
		v4l2-ctl -d "$dev" --list-formats 2>/dev/null | grep -q "'GREY'" || continue
		for link in /dev/v4l/by-path/*; do
			[[ "$link" == *usbv2* ]] && continue
			if [[ "$(readlink -f "$link")" == "$dev" ]]; then
				echo "$link"
				return 0
			fi
		done
		echo "$dev"
		return 0
	done
	return 1
}

# ---------------------------------------------------------------- uninstall

if [[ $UNINSTALL -eq 1 ]]; then
	[[ $EUID -ne 0 ]] || die "run this as your normal user, not root"
	bold "Turning face unlock off in PAM"
	if [[ -x "$HELPER" ]]; then
		sudo "$HELPER" pam-disable "${SERVICES[@]}"
	else
		# The package is already gone: drop our lines by hand
		for service in "${SERVICES[@]}"; do
			if grep -qsE '^[^#]*pam_linux_hello_camera\.so|^[^#].*# linux-hello-camera' "/etc/pam.d/$service"; then
				sudo sed -i -E '/^[^#]*pam_linux_hello_camera\.so|^[^#].*# linux-hello-camera/d' "/etc/pam.d/$service"
				info "Removed face unlock from /etc/pam.d/$service"
			fi
		done
	fi
	bold "Removing the package"
	if installed linux-hello-camera; then
		sudo pacman -Rns linux-hello-camera
	fi
	info "Enrolled faces in /var/lib/linux-hello-camera were kept. Delete that folder to remove them."
	exit 0
fi

# ---------------------------------------------------------------- install

[[ $EUID -ne 0 ]] || die "run this as your normal user, not root (makepkg refuses to run as root)"
command -v pacman >/dev/null || die "this script is for Arch-based distributions (pacman not found)"

bold "Linux Hello Camera installer"
info "This will build and install Linux Hello Camera from $REPO_DIR."
info "The build downloads ONNX Runtime and the face models, so it needs internet."
info "Nothing is added to PAM yet. You choose that in the app afterwards."
confirm "Continue?" || exit 0

bold "Installing build tools and dependencies"
sudo pacman -S --needed --noconfirm base-devel git cmake ninja v4l-utils python-yaml \
	python python-gobject gtk4 libadwaita polkit ffmpeg libjpeg-turbo pam

mkdir -p "$WORK_DIR"

# Build first, so nothing on the system changes if the build fails. Out of tree, so the
# repository stays clean.
bold "Building Linux Hello Camera"
export BUILDDIR="$WORK_DIR/build" PKGDEST="$WORK_DIR/pkg" SRCDEST="$WORK_DIR/src"
(
	cd "$REPO_DIR"
	makepkg --syncdeps --force --clean --noconfirm
)
pkgfile="$(cd "$REPO_DIR" && makepkg --packagelist | head -n1)"
[[ -f "$pkgfile" ]] || die "the build didn't produce a package"

bold "Installing Linux Hello Camera"
sudo pacman -U --noconfirm "$pkgfile"

bold "Configuring the camera"
current="$(sed -n 's/^camera: *//p' "$CONFIG" 2>/dev/null | head -n1 | tr -d "\"'")"
if [[ -n "$current" && -e "$current" ]]; then
	info "Already using $current"
elif camera="$(find_ir_camera)"; then
	info "Found an IR camera: $camera"
	sudo "$HELPER" set "camera=$camera" >/dev/null
	info "Saved it as the camera"
else
	warn "no IR camera found. Pick a camera in the app (Camera tab)."
fi

bold "Done"
cat <<-EOF
	    Your face is stored as encrypted templates, never as photos (locked to this PC's security
	    chip when it has a TPM 2.0; you can change that in the app).

	    Next steps in "Linux Hello Camera" (app grid, or run: linux-hello-camera):
	      1. Face Unlock tab -> Add your face, then Test
	      2. Turn on "sudo" and try it in a new terminal (keep a root shell open until it works)
	      3. Then turn on the login / lock screen switches and "Administrator prompts"
EOF
if [[ -n "${WAYLAND_DISPLAY:-}${DISPLAY:-}" ]] && confirm "Open Linux Hello Camera now?"; then
	setsid -f linux-hello-camera >/dev/null 2>&1
fi
