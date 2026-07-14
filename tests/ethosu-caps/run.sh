#!/usr/bin/env bash
#
# Ethos-U65 capabilities round-trip test for the i.MX93 QEMU machine.
#
# This is the first step of the fork-only inference demo. It proves the full
# A55 -> M33 -> NPU path end to end: the guest opens /dev/ethosu0 (which makes
# the kernel boot the M33 on demand via the i.MX SiP RPROC SMC), then issues
# ETHOSU_IOCTL_CAPABILITIES_REQ. The request travels over MU/rpmsg to the M33
# firmware, which reads the modelled NPU's ID/CONFIG registers and replies. The
# guest tool (ethosu_caps) prints what the NPU reported.
#
# PASS = the ethosu_caps tool prints "CAPABILITIES_REQ OK" and there is no oops.
#
# The M33 firmware (ethosu_firmware) must be in the rootfs /lib/firmware.
# Override paths via env: KERNEL=/path/Image DTB=/path.dtb BASE_INITRD=...
set -u

HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
DEPLOY=${DEPLOY:-$HOME/Documents/nxp/linux/imx-yocto-bsp/build-imx93/tmp/deploy/images/imx93evk}

QEMU=${QEMU:-$REPO/build-imx93/qemu-system-aarch64}
KERNEL=${KERNEL:-$DEPLOY/Image}
DTB=${DTB:-$DEPLOY/imx93-11x11-evk.dtb}
BASE_INITRD=${BASE_INITRD:-$HOME/Documents/nxp/imx93-initramfs.cpio.gz}
APP=${APP:-$HERE/ethosu_caps}

need() { [ -e "$2" ] || { echo "error: $3 not found: $2" >&2; exit 1; }; }
need QEMU "$QEMU" "qemu (build it)"; need KERNEL "$KERNEL" "kernel"
need DTB "$DTB" "dtb"; need BASE_INITRD "$BASE_INITRD" "initrd"

if [ ! -x "$APP" ]; then
    echo "building ethosu_caps..."
    aarch64-linux-gnu-gcc -static -O2 -Wall -o "$APP" "$HERE/ethosu_caps.c" || exit 1
fi

TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
cp "$APP" "$TMP/ethosu_caps"
cat > "$TMP/myinit" <<'EOF'
#!/bin/sh
PATH=/sbin:/usr/sbin:/bin:/usr/bin; export PATH
mount -t proc proc /proc; mount -t sysfs sysfs /sys; mount -t devtmpfs devtmpfs /dev
sleep 3
echo "=== ETHOSU CAPS TEST: querying NPU capabilities ==="
/ethosu_caps /dev/ethosu0
echo "--- dmesg (ethosu) ---"
dmesg | grep -iE "Booting fw image ethosu|rpmsg-ethosu-channel|remote processor.*up" | tail
if dmesg | grep -q "Internal error: Oops"; then
    echo "RESULT: FAIL - kernel oops"
fi
echo "=== ETHOSU CAPS TEST DONE ==="
while true; do sleep 5; done
EOF
chmod +x "$TMP/myinit"
( cd "$TMP" && printf 'myinit\nethosu_caps\n' | cpio -o -H newc 2>/dev/null > o.cpio )
cat "$BASE_INITRD" "$TMP/o.cpio" > "$TMP/c.cpio.gz"

M33CON=${M33CON:-/tmp/ethosu-caps-m33-console.log}
echo "M33 FreeRTOS console -> $M33CON"
set -x
exec "$QEMU" -M imx93-11x11-evk -audio driver=none -m 4G -display none \
    -kernel "$KERNEL" -dtb "$DTB" -initrd "$TMP/c.cpio.gz" \
    -append "console=ttyLP0,115200 cpuidle.off=1 rdinit=/myinit ignore_loglevel" \
    -serial mon:stdio -serial "file:$M33CON"
