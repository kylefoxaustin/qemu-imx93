#!/usr/bin/env bash
#
# FlexIO-as-I2C functional check for the i.MX93 QEMU machine.
#
# Boots the imx93-11x11-evk-flexio-i2c device tree (which routes an extra I2C
# master through the FlexIO block), attaches a tmp105 sensor on the FlexIO I2C
# bus, chains a tiny overlay initramfs (/myinit) onto the imx-image-core
# rootfs, and exercises the bus from Linux. Expect:
#
#   /dev/i2c-8 -> 425c0000.flexio:i2c-master
#   i2cdetect -y 8 finds 0x49
#   temp reg 0x00 (word): 0x0000
#   config reg 0x01 readback: 0x60   (after writing 0x60)
#
# i2cdetect may also show 0x4a (the neighbour of a present device): a cosmetic
# artifact of the i2c-flexio driver's post-success shifter state, not a model
# datapath error - the read/write round trip above is byte-exact.
#
# Override any path via env: KERNEL=/path/Image DTB=/path.dtb BASE_INITRD=...
set -u

HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
DEPLOY=${DEPLOY:-$HOME/Documents/nxp/linux/imx-yocto-bsp/build-imx93/tmp/deploy/images/imx93evk}

QEMU=${QEMU:-$REPO/build-imx93/qemu-system-aarch64}
KERNEL=${KERNEL:-$DEPLOY/Image}
DTB=${DTB:-$DEPLOY/imx93-11x11-evk-flexio-i2c.dtb}
BASE_INITRD=${BASE_INITRD:-$HOME/Documents/nxp/imx93-initramfs.cpio.gz}

need() { [ -e "$2" ] || { echo "error: $3 not found: $2" >&2; exit 1; }; }
need QEMU        "$QEMU"        "qemu-system-aarch64 (build it first)"
need KERNEL      "$KERNEL"      "kernel Image"
need DTB         "$DTB"         "flexio-i2c device tree"
need BASE_INITRD "$BASE_INITRD" "base imx-image-core initramfs cpio.gz"

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
install -m755 "$HERE/myinit" "$TMP/myinit"
( cd "$TMP" && echo myinit | cpio -o -H newc 2>/dev/null > overlay.cpio )
cat "$BASE_INITRD" "$TMP/overlay.cpio" > "$TMP/combined.cpio.gz"

set -x
exec "$QEMU" -M imx93-11x11-evk -audio driver=none -m 4G -display none \
    -device tmp105,bus=flexio1-i2c,address=0x49 \
    -kernel "$KERNEL" -dtb "$DTB" -initrd "$TMP/combined.cpio.gz" \
    -append "console=ttyLP0,115200 cpuidle.off=1 rdinit=/myinit ignore_loglevel" \
    -serial mon:stdio -serial null
