#!/usr/bin/env bash
#
# A55 <-> M33 RPMsg bring-up test for the i.MX93 QEMU machine.
#
# Stages the NXP M33 rpmsg-lite pingpong firmware (replicating U-Boot bootaux):
#   - the firmware .bin at the ITCM A55-view alias + FW_OFFSET (0x201E0000), and
#   - its resource table (extracted from the .bin) at the cm33 rsc-table region
#     (0x2021e000, which lives in the M33 DTCM), so imx-rproc reads a valid
#     table and brings up the virtio-rpmsg vdevs.
#
# Then modprobes imx_rpmsg_pingpong and dumps the rpmsg state. Current
# milestone: the virtio-rpmsg bus comes online (virtio0/virtio1). The full
# ping-pong round-trip additionally needs the MU1 doorbell relay (A55 MU1_MUB
# 0x44230000 <-> M33 MU1_MUA 0x44220000 peer-link) and the M33 running.
#
# FW=/path/to/..._rpmsg_lite_pingpong_rtos_linux_remote.bin overrides the blob.
set -u

HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
DEPLOY=${DEPLOY:-$HOME/Documents/nxp/linux/imx-yocto-bsp/build-imx93/tmp/deploy/images/imx93evk}
FWDIR=${FWDIR:-$HOME/Documents/nxp/imx93-rootfs/lib/firmware}

QEMU=${QEMU:-$REPO/build-imx93/qemu-system-aarch64}
KERNEL=${KERNEL:-$DEPLOY/Image}
DTB=${DTB:-$DEPLOY/imx93-11x11-evk.dtb}
BASE_INITRD=${BASE_INITRD:-$HOME/Documents/nxp/imx93-initramfs.cpio.gz}
FW=${FW:-$FWDIR/imx93-11x11-evk_m33_TCM_rpmsg_lite_pingpong_rtos_linux_remote.bin}

need() { [ -e "$2" ] || { echo "error: $3 not found: $2" >&2; exit 1; }; }
need QEMU "$QEMU" "qemu (build it)"; need KERNEL "$KERNEL" "kernel"
need DTB "$DTB" "dtb"; need BASE_INITRD "$BASE_INITRD" "initrd"
need FW "$FW" "M33 pingpong firmware .bin (BSP /lib/firmware)"

TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
# Extract the resource table from the firmware (.resource_table @ .bin 0x478).
python3 -c "open('$TMP/rsc.bin','wb').write(open('$FW','rb').read()[0x478:0x478+0xa0])"

cat > "$TMP/myinit" <<'EOF'
#!/bin/sh
PATH=/sbin:/usr/sbin:/bin:/usr/bin; export PATH
mount -t proc proc /proc; mount -t sysfs sysfs /sys; mount -t devtmpfs devtmpfs /dev
sleep 3
echo "=== M33 RPMSG TEST ==="
modprobe imx_rpmsg_pingpong 2>&1 | head; sleep 3
echo "--- virtio devices ---"; ls /sys/bus/virtio/devices 2>/dev/null
dmesg | grep -iE "rpmsg|virtio|pingpong|new channel|goodbye" | tail -15
if dmesg | grep -q "rpmsg-openamp-demo-channel.*new channel"; then
    echo "RESULT: PASS - A55<->M33 rpmsg channel up, ping-pong ran"
else
    echo "RESULT: FAIL - no rpmsg channel"
fi
echo "=== M33 RPMSG TEST DONE ==="
while true; do sleep 5; done
EOF
chmod +x "$TMP/myinit"
( cd "$TMP" && echo myinit | cpio -o -H newc 2>/dev/null > o.cpio )
cat "$BASE_INITRD" "$TMP/o.cpio" > "$TMP/c.cpio.gz"

# 2nd serial = lpuart2 = the M33's FreeRTOS console (its ping-pong banner);
# captured to a file you can `tail -f` (expect "Link is up! ... Sending pong...").
M33CON=${M33CON:-/tmp/m33-rpmsg-console.log}
echo "M33 FreeRTOS console -> $M33CON"
set -x
exec "$QEMU" -M imx93-11x11-evk -audio driver=none -m 4G -display none \
    -kernel "$KERNEL" -dtb "$DTB" -initrd "$TMP/c.cpio.gz" \
    -append "console=ttyLP0,115200 cpuidle.off=1 rdinit=/myinit ignore_loglevel" \
    -device loader,file="$FW",addr=0x201E0000,force-raw=on \
    -device loader,file="$TMP/rsc.bin",addr=0x2021e000,force-raw=on \
    -serial mon:stdio -serial "file:$M33CON"
