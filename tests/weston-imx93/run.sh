#!/usr/bin/env bash
#
# Weston (Wayland) desktop on the i.MX93 QEMU machine, shown on the emulated
# HDMI/LCDIFv3 output. Boots a core-image-weston rootfs from a .wic disk image
# as a virtual SD card; virtio-keyboard + virtio-tablet give keyboard + pointer
# so you can drive the desktop in the GTK window.
#
# IMPORTANT — the i.MX93 has no 3D GPU and our PXP/G2D 2D engine is not modelled,
# so the NXP default (weston.ini "[core] use-g2d=true") composites via G2D and
# renders nothing (black screen). The rootfs referenced here has been patched to
# "use-g2d=false", which makes Weston fall back to software rendering
# (Mesa softpipe / pixman) and the desktop appears. To reproduce from a fresh
# build, set use-g2d=false in /etc/xdg/weston/weston.ini in the rootfs (a
# weston bbappend shipping that weston.ini is the build-time way).
#
# The .wic must be a power-of-2 size for the SD model: if you regenerate it from
# the BSP, `zstd -d core-image-weston-*.wic.zst -o weston.wic && qemu-img resize
# -f raw weston.wic 1G`.
#
# Override paths via env: KERNEL=, DTB=, WIC=, QEMU=, DISPLAY_BACKEND=
set -u

HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
DEPLOY=${DEPLOY:-$HOME/Documents/nxp/linux/imx-yocto-bsp/build-imx93/tmp/deploy/images/imx93evk}

QEMU=${QEMU:-$REPO/build-imx93/qemu-system-aarch64}
KERNEL=${KERNEL:-$DEPLOY/Image}
DTB=${DTB:-$DEPLOY/imx93-11x11-evk.dtb}
WIC=${WIC:-$HOME/Documents/nxp/imx93-weston.wic}   # use-g2d=false, sized to 1G
DISPLAY_BACKEND=${DISPLAY_BACKEND:-gtk}

need() { [ -e "$2" ] || { echo "error: $3 not found: $2" >&2; exit 1; }; }
need QEMU   "$QEMU"   "qemu-system-aarch64 (build it first)"
need KERNEL "$KERNEL" "kernel Image"
need DTB    "$DTB"    "device tree"
need WIC    "$WIC"    "core-image-weston .wic disk (use-g2d=false, 1G)"

set -x
exec "$QEMU" -M imx93-11x11-evk -audio driver=none -m 4G -display "$DISPLAY_BACKEND" \
    -kernel "$KERNEL" -dtb "$DTB" \
    -drive if=sd,file="$WIC",format=raw \
    -append "console=ttyLP0,115200 root=/dev/mmcblk0p2 rootwait rw cpuidle.off=1" \
    -device virtio-keyboard-device -device virtio-tablet-device \
    -serial mon:stdio -serial null
