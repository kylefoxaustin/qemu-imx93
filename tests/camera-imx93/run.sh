#!/usr/bin/env bash
#
# Camera capture check for the i.MX93 QEMU machine.
#
# Boots the *mt9m114* device-tree variant (the parallel-camera path: MT9M114
# sensor -> parallel-CSI -> ISI crossbar -> mxc_isi.0 -> /dev/video0),
# cross-compiles the V4L2 capture oracle (v4l2_cap.c), chains it plus /myinit
# into an overlay initramfs on top of the imx-image-core rootfs, and captures
# real frames. The oracle enables the (default-disabled) sensor link, pushes
# the sensor format down the pipe so media-core link validation passes, then
# streams MMAP buffers. The ISI model DMAs a moving test pattern into the
# ping-pong buffers and raises the frame-stored IRQ. Expect:
#
#   /dev/media0  /dev/v4l-subdev0..3  /dev/video0  /dev/video1
#   mt9m114: MT9M114 is found
#   CAMERA-CAP[/dev/video0]: PASS (5/5 frames)
#
# Override paths via env: KERNEL=/path/Image DTB=/path.dtb BASE_INITRD=...
# CROSS=aarch64-linux-gnu- to pick a different cross toolchain prefix.
set -u

HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
DEPLOY=${DEPLOY:-$HOME/Documents/nxp/linux/imx-yocto-bsp/build-imx93/tmp/deploy/images/imx93evk}

QEMU=${QEMU:-$REPO/build-imx93/qemu-system-aarch64}
KERNEL=${KERNEL:-$DEPLOY/Image}
DTB=${DTB:-$DEPLOY/imx93-11x11-evk-mt9m114.dtb}
BASE_INITRD=${BASE_INITRD:-$HOME/Documents/nxp/imx93-initramfs.cpio.gz}
CROSS=${CROSS:-aarch64-linux-gnu-}

need() { [ -e "$2" ] || { echo "error: $3 not found: $2" >&2; exit 1; }; }
need QEMU        "$QEMU"        "qemu-system-aarch64 (build it first)"
need KERNEL      "$KERNEL"      "kernel Image"
need DTB         "$DTB"         "mt9m114 device tree (imx93-11x11-evk-mt9m114.dtb)"
need BASE_INITRD "$BASE_INITRD" "base imx-image-core initramfs cpio.gz"
command -v "${CROSS}gcc" >/dev/null || {
    echo "error: ${CROSS}gcc not found (set CROSS=)" >&2; exit 1; }

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
"${CROSS}gcc" -O2 -Wall -static -o "$TMP/v4l2_cap" "$HERE/v4l2_cap.c"
install -m755 "$HERE/myinit" "$TMP/myinit"
( cd "$TMP" && printf 'myinit\nv4l2_cap\n' | cpio -o -H newc 2>/dev/null \
    > overlay.cpio )
cat "$BASE_INITRD" "$TMP/overlay.cpio" > "$TMP/combined.cpio.gz"

set -x
exec "$QEMU" -M imx93-11x11-evk -audio driver=none -m 4G -display none \
    -kernel "$KERNEL" -dtb "$DTB" -initrd "$TMP/combined.cpio.gz" \
    -append "console=ttyLP0,115200 cpuidle.off=1 rdinit=/myinit ignore_loglevel" \
    -serial mon:stdio -serial null
