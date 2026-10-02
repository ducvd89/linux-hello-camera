# Local package for Linux Hello Camera. Build with: makepkg -si   (or ./install.sh)
pkgname=linux-hello-camera
pkgver=0.9.1
pkgrel=1
pkgdesc="Face unlock for Linux with an IR camera, like Windows Hello"
arch=('x86_64')
url="https://github.com/ducvd89/linux-hello-camera"
license=('MIT')
install=linux-hello-camera.install
depends=('python' 'python-gobject' 'gtk4' 'libadwaita' 'polkit' 'ffmpeg' 'v4l-utils'
         'python-yaml' 'libjpeg-turbo' 'pam' 'systemd')
# The engine's CMake build downloads ONNX Runtime and a few small libraries (git),
# so building needs network access.
makedepends=('cmake' 'ninja' 'git')
backup=('etc/linux-hello-camera/config.yaml')

_models=https://github.com/TickLabVN/biopass/releases/download/1.4.1
source=("$_models/yolov8n-face.onnx"
        "$_models/edgeface_s_gamma_05.onnx"
        "$_models/edgeface_xs_gamma_06.onnx"
        "$_models/minifas_v2.onnx"
        "$_models/mobilenetv3_antispoof.onnx")
sha256sums=('3e81709ceaae911db2dd23bed39aa1fc980b6320e8a8bbf8b587b2a8feaa5a5a'
            'cdc2ced431b4bf071c92f2fe8bb6ed4b5017be4175b4375eaa6e14b255f831ce'
            '619ffe6f9df592e250e900ed135c2de3d620f6bc0a913f7d0432fa0735073a53'
            'af2381b88f38769222ed93379e12444e2a50814575de1c46170de570c55a42b6'
            'a90d32b1f217658d1eb1ea05d9248d07fbd05c8ee41b4baa4c36b5cde30d40f4')

build() {
	cmake -S "$startdir/engine" -B build -G Ninja \
		-DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr \
		-DLHC_VERSION="$pkgver" -DBUILD_TESTING=OFF -Wno-dev
	cmake --build build
}

package() {
	local app="$startdir/app" lib=/usr/lib/linux-hello-camera

	# Engine: PAM module, helper binary and the bundled ONNX Runtime
	DESTDIR="$pkgdir" cmake --install build

	# Models
	local model
	for model in "${source[@]}"; do
		install -Dm644 "$srcdir/${model##*/}" "$pkgdir/usr/share/linux-hello-camera/models/${model##*/}"
	done

	# The app and everything the PAM stack runs on its behalf
	install -Dm755 "$app/linux_hello_camera.py" "$pkgdir$lib/linux_hello_camera.py"
	install -Dm644 "$app/linux_hello_camera_common.py" "$pkgdir$lib/linux_hello_camera_common.py"
	install -Dm755 "$app/settings-helper" "$pkgdir$lib/settings-helper"
	install -Dm755 "$app/input-gate" "$pkgdir$lib/input-gate"
	install -Dm755 "$app/confirm-broker" "$pkgdir$lib/confirm-broker"
	install -Dm755 "$app/confirm-dialog" "$pkgdir$lib/confirm-dialog"
	install -Dm755 "$app/confirm-hook" "$pkgdir$lib/confirm-hook"
	install -d "$pkgdir/usr/bin"
	ln -s "$lib/linux_hello_camera.py" "$pkgdir/usr/bin/linux-hello-camera"
	install -Dm644 "$app/io.github.linux_hello_camera.desktop" \
		"$pkgdir/usr/share/applications/io.github.linux_hello_camera.desktop"
	install -Dm644 "$app/io.github.linux_hello_camera.policy" \
		"$pkgdir/usr/share/polkit-1/actions/io.github.linux_hello_camera.policy"

	# Default config (kept across upgrades), and the drop-in that lets polkit's sandboxed password
	# helper create the confirmation broker's socket in /run/linux-hello-camera
	install -Dm644 "$app/config.yaml" "$pkgdir/etc/linux-hello-camera/config.yaml"
	install -Dm644 "$app/10-linux-hello-camera.conf" \
		"$pkgdir/usr/lib/systemd/system/polkit-agent-helper@.service.d/10-linux-hello-camera.conf"

	install -Dm644 "$startdir/LICENSE" "$pkgdir/usr/share/licenses/$pkgname/LICENSE"
	install -Dm644 "$startdir/engine/LICENSE.biopass" "$pkgdir/usr/share/licenses/$pkgname/LICENSE.biopass"
}
