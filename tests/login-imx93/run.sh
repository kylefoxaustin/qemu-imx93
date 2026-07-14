#!/usr/bin/env bash
#
# Interactive login on the i.MX93 QEMU machine — over the serial console, with
# the HDMI framebuffer (LCDIFv3 -> MIPI-DSI -> ADV7535) mirroring the screen.
#
# It chains a tiny overlay initramfs (just /myinit, see ./myinit) onto the
# imx-image-core rootfs. /myinit brings up agetty on both tty1 (the framebuffer
# console, shown on the emulated HDMI output) and ttyLP0 (serial). root has an
# empty password (the BSP image ships pam_unix nullok + tty1/ttyLP0 in
# securetty), so at "imx93evk login:" just type:  root  <Enter>.
#
# Two ways to interact:
#   - Type directly in the GUI window (-display gtk): a virtio-keyboard feeds
#     tty1, so keystrokes land on the HDMI framebuffer console. (The board adds
#     virtio-mmio transports + DTB nodes; the kernel's virtio_input binds the
#     "-device virtio-keyboard-device" below.)
#   - Or type in THIS terminal (-serial mon:stdio) for the serial console.
#
# Override any path via env:  KERNEL=/path/Image DTB=/path.dtb BASE_INITRD=...
set -u

HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
DEPLOY=${DEPLOY:-$HOME/Documents/nxp/linux/imx-yocto-bsp/build-imx93/tmp/deploy/images/imx93evk}

QEMU=${QEMU:-$REPO/build-imx93/qemu-system-aarch64}
KERNEL=${KERNEL:-$DEPLOY/Image}
DTB=${DTB:-$DEPLOY/imx93-11x11-evk.dtb}
# imx-image-core rootfs cpio (the one with busybox + agetty + login.shadow).
BASE_INITRD=${BASE_INITRD:-$HOME/Documents/nxp/imx93-initramfs.cpio.gz}
DISPLAY_BACKEND=${DISPLAY_BACKEND:-gtk}   # gtk | sdl | none

need() { [ -e "$2" ] || { echo "error: $3 not found: $2" >&2; exit 1; }; }
need QEMU       "$QEMU"       "qemu-system-aarch64 (build it first)"
need KERNEL     "$KERNEL"     "kernel Image"
need DTB        "$DTB"        "device tree"
need BASE_INITRD "$BASE_INITRD" "base imx-image-core initramfs cpio.gz"

# Build the overlay cpio (/myinit) and chain it after the base rootfs. The
# kernel unpacks concatenated cpio archives; the later one wins, so /myinit
# becomes init via rdinit=.
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
install -m755 "$HERE/myinit" "$TMP/myinit"
( cd "$TMP" && echo myinit | cpio -o -H newc 2>/dev/null > overlay.cpio )
cat "$BASE_INITRD" "$TMP/overlay.cpio" > "$TMP/combined.cpio.gz"

set -x
exec "$QEMU" -M imx93-11x11-evk -audio driver=none -m 4G -display "$DISPLAY_BACKEND" \
    -kernel "$KERNEL" -dtb "$DTB" -initrd "$TMP/combined.cpio.gz" \
    -append "console=tty0 console=ttyLP0,115200 cpuidle.off=1 rdinit=/myinit quiet loglevel=3" \
    -device virtio-keyboard-device -device virtio-tablet-device \
    -serial mon:stdio -serial null
