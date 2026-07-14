#!/usr/bin/env bash
#
# Ethos-U65 on-demand boot test for the i.MX93 QEMU machine.
#
# Unlike tests/m33-rpmsg (which pre-loads M33 firmware via -device loader),
# this exercises the *real* flow: Linux boots the M33 itself. Opening
# /dev/ethosu0 makes the ethosu driver rproc_boot() the cm33 - it loads
# /lib/firmware/ethosu_firmware and issues the i.MX SiP RPROC START SMC, which
# the machine services by releasing the M33. The firmware then initialises the
# Ethos-U65 (modelled NPU @ 0x4a900000) and brings up "rpmsg-ethosu-channel".
#
# PASS = the firmware is booted by Linux, the channel is created, and there is
# no kernel oops (the on-demand init_completion ordering that the pre-loaded
# path violated). NOTE: no -device loader for the M33 here - that is the point.
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

need() { [ -e "$2" ] || { echo "error: $3 not found: $2" >&2; exit 1; }; }
need QEMU "$QEMU" "qemu (build it)"; need KERNEL "$KERNEL" "kernel"
need DTB "$DTB" "dtb"; need BASE_INITRD "$BASE_INITRD" "initrd"

TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
cat > "$TMP/myinit" <<'EOF'
#!/bin/sh
PATH=/sbin:/usr/sbin:/bin:/usr/bin; export PATH
mount -t proc proc /proc; mount -t sysfs sysfs /sys; mount -t devtmpfs devtmpfs /dev
sleep 3
echo "=== ETHOSU ON-DEMAND TEST: opening /dev/ethosu0 ==="
head -c 1 /dev/ethosu0 >/dev/null 2>&1 &
sleep 5
dmesg | grep -iE "Booting fw image ethosu|rpmsg-ethosu-channel|remote processor.*up" | tail
if dmesg | grep -q "creating channel rpmsg-ethosu-channel" && \
   ! dmesg | grep -q "Internal error: Oops"; then
    echo "RESULT: PASS - Linux booted the M33, ethosu channel up, no oops"
else
    echo "RESULT: FAIL"
fi
echo "=== ETHOSU ON-DEMAND TEST DONE ==="
while true; do sleep 5; done
EOF
chmod +x "$TMP/myinit"
( cd "$TMP" && echo myinit | cpio -o -H newc 2>/dev/null > o.cpio )
cat "$BASE_INITRD" "$TMP/o.cpio" > "$TMP/c.cpio.gz"

M33CON=${M33CON:-/tmp/ethosu-m33-console.log}
echo "M33 FreeRTOS console -> $M33CON (expect 'Initialize Arm Ethos-U / RPMSG_LITE is link up')"
set -x
exec "$QEMU" -M imx93-11x11-evk -audio driver=none -m 4G -display none \
    -kernel "$KERNEL" -dtb "$DTB" -initrd "$TMP/c.cpio.gz" \
    -append "console=ttyLP0,115200 cpuidle.off=1 rdinit=/myinit ignore_loglevel" \
    -serial mon:stdio -serial "file:$M33CON"
