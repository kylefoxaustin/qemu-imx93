#!/usr/bin/env bash
#
# I3C functional check for the i.MX93 QEMU machine.
#
# Boots the imx93-11x11-evk-i3c device tree, which moves the wm8962 codec off
# LPI2C onto the I3C1 bus (the Silvaco "silvaco,i3c-master-v1" master at
# 0x44330000) as a legacy-I2C target. The model (hw/i3c/svc_i3c.c) bridges I3C
# to a real QEMU I2C bus with a wm8962 attached at 0x1a, chains a tiny overlay
# initramfs (/myinit) onto the imx-image-core rootfs, and exercises it from
# Linux. Expect:
#
#   /sys/bus/i3c/devices/i3c-0                (the I3C master's bus registers)
#   wm8962 8-001a: customer id 0 revision A   (codec probes over I3C, on i2c-8)
#   card: wm8962-audio                        (the ASoC card comes up)
#
# So I3C is a functional master that bridges to real I2C targets, not just a
# registration stub.
#
# Override any path via env: KERNEL=/path/Image DTB=/path.dtb BASE_INITRD=...
set -u

HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
DEPLOY=${DEPLOY:-$HOME/Documents/nxp/linux/imx-yocto-bsp/build-imx93/tmp/deploy/images/imx93evk}

QEMU=${QEMU:-$REPO/build-imx93/qemu-system-aarch64}
KERNEL=${KERNEL:-$DEPLOY/Image}
DTB=${DTB:-$DEPLOY/imx93-11x11-evk-i3c.dtb}
BASE_INITRD=${BASE_INITRD:-$HOME/Documents/nxp/imx93-initramfs.cpio.gz}

need() { [ -e "$2" ] || { echo "error: $3 not found: $2" >&2; exit 1; }; }
need QEMU        "$QEMU"        "qemu-system-aarch64 (build it first)"
need KERNEL      "$KERNEL"      "kernel Image"
need DTB         "$DTB"         "i3c device tree (imx93-11x11-evk-i3c.dtb)"
need BASE_INITRD "$BASE_INITRD" "base imx-image-core initramfs cpio.gz"

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
install -m755 "$HERE/myinit" "$TMP/myinit"
( cd "$TMP" && echo myinit | cpio -o -H newc 2>/dev/null > overlay.cpio )
cat "$BASE_INITRD" "$TMP/overlay.cpio" > "$TMP/combined.cpio.gz"

set -x
exec "$QEMU" -M imx93-11x11-evk -m 4G -display none \
    -kernel "$KERNEL" -dtb "$DTB" -initrd "$TMP/combined.cpio.gz" \
    -append "console=ttyLP0,115200 cpuidle.off=1 rdinit=/myinit ignore_loglevel" \
    -serial mon:stdio -serial null
