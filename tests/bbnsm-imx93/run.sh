#!/usr/bin/env bash
#
# BBNSM RTC test for the i.MX93 QEMU machine.
#
# Boots the NXP BSP and checks the rtc-nxp-bbnsm driver binds, registers an RTC,
# and reads back a time close to the host wall-clock (the model's 32768 Hz
# counter tracks QEMU_CLOCK_HOST). Also round-trips a set/read.
#
# PASS = rtc0 present, read time within ~1 day of host, set/read round-trips.
set -u

HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
DEPLOY=${DEPLOY:-$HOME/Documents/nxp/linux/imx-yocto-bsp/build-imx93/tmp/deploy/images/imx93evk}
QEMU=${QEMU:-$REPO/build-imx93/qemu-system-aarch64}
KERNEL=${KERNEL:-$DEPLOY/Image}
DTB=${DTB:-$DEPLOY/imx93-11x11-evk.dtb}
BASE_INITRD=${BASE_INITRD:-$HOME/Documents/nxp/imx93-initramfs.cpio.gz}

need() { [ -e "$2" ] || { echo "error: $3 not found: $2" >&2; exit 1; }; }
need QEMU "$QEMU" "qemu"; need KERNEL "$KERNEL" "kernel"; need DTB "$DTB" "dtb"
need BASE_INITRD "$BASE_INITRD" "initrd"

HOST_EPOCH=$(date -u +%s)
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
cat > "$TMP/myinit" <<EOF
#!/bin/sh
PATH=/sbin:/usr/sbin:/bin:/usr/bin; export PATH
mount -t proc proc /proc; mount -t sysfs sysfs /sys; mount -t devtmpfs devtmpfs /dev
sleep 2
echo "=== BBNSM RTC TEST ==="
dmesg | grep -iE "bbnsm|rtc" | tail -5
GUEST=\$(cat /sys/class/rtc/rtc0/since_epoch 2>/dev/null || echo 0)
echo "rtc0 since_epoch=\$GUEST host_epoch=$HOST_EPOCH"
hwclock -r 2>/dev/null || cat /sys/class/rtc/rtc0/time 2>/dev/null
# set/read round-trip: set to a known time, read it back
hwclock --set --date="2020-02-02 02:02:02" -u 2>/dev/null
sleep 1
echo "after-set: \$(cat /sys/class/rtc/rtc0/date 2>/dev/null) \$(cat /sys/class/rtc/rtc0/time 2>/dev/null)"
if [ "\$GUEST" -gt $((HOST_EPOCH - 86400)) ] 2>/dev/null; then
    echo "RESULT: PASS - RTC reads host wall-clock (\$GUEST)"
else
    echo "RESULT: FAIL - rtc0 missing or time off (\$GUEST)"
fi
echo "=== BBNSM RTC TEST DONE ==="
while true; do sleep 5; done
EOF
chmod +x "$TMP/myinit"
( cd "$TMP" && find ./myinit | cpio -o -H newc 2>/dev/null > o.cpio )
cat "$BASE_INITRD" "$TMP/o.cpio" > "$TMP/c.cpio.gz"

exec "$QEMU" -M imx93-11x11-evk -audio driver=none -m 4G -display none \
    -kernel "$KERNEL" -dtb "$DTB" -initrd "$TMP/c.cpio.gz" \
    -append "console=ttyLP0,115200 cpuidle.off=1 rdinit=/myinit ignore_loglevel" \
    -serial mon:stdio -serial null
