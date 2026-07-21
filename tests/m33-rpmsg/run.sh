#!/usr/bin/env bash
#
# A55 <-> M33 RPMsg ping-pong round-trip test for the i.MX93 QEMU machine.
#
# Stages the NXP M33 rpmsg-lite pingpong firmware (replicating U-Boot bootaux):
#   - the firmware .bin at the ITCM A55-view alias + FW_OFFSET (0x201E0000), and
#   - its resource table (extracted from the .bin) at the cm33 rsc-table region
#     (0x2021e000, in the M33 DTCM), so imx-rproc reads a valid table and brings
#     up the virtio-rpmsg vdevs.
#
# The M33 runs the firmware concurrently with Linux, and the two exchange
# messages over the shared vrings paced by the MU1 doorbell relay (A55 MU1_MUB
# 0x44230000 <-> M33 MU1_MUA 0x44220000 peer-link). Linux's imx_rpmsg_pingpong
# sends a counter, the M33 replies it incremented, for ~100 rounds.
#
# PASS asserts the FULL round-trip, both endpoints:
#   - A55: the rpmsg channel comes up AND Linux logs a sustained stream of
#     "get N (src ...)" replies (the M33's pongs coming back), and
#   - M33: its FreeRTOS console reaches "Ping pong done" (the exchange finished).
# Channel-up alone is NOT enough - a doorbell that never relays would leave the
# channel created but no message ever flowing.
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

# How many "get N" replies Linux must log to call the round-trip sustained (the
# demo does ~100; require a solid fraction so a one-off does not pass).
MIN_ROUNDS=${MIN_ROUNDS:-20}

skip() { echo "SKIP: $1"; exit 0; }
[ -e "$QEMU" ]        || skip "qemu not built: $QEMU"
[ -e "$KERNEL" ]      || skip "kernel not found: $KERNEL"
[ -e "$DTB" ]         || skip "dtb not found: $DTB"
[ -e "$BASE_INITRD" ] || skip "base initramfs not found: $BASE_INITRD"
[ -e "$FW" ]          || skip "M33 pingpong firmware not found: $FW"
command -v python3 >/dev/null || skip "python3 not found"

TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
A55LOG="$TMP/a55.log"; M33LOG="$TMP/m33.log"

# Extract the resource table from the firmware (.resource_table @ .bin 0x478).
python3 -c "open('$TMP/rsc.bin','wb').write(open('$FW','rb').read()[0x478:0x478+0xa0])"

cat > "$TMP/myinit" <<'EOF'
#!/bin/sh
PATH=/sbin:/usr/sbin:/bin:/usr/bin; export PATH
mount -t proc proc /proc; mount -t sysfs sysfs /sys; mount -t devtmpfs devtmpfs /dev
sleep 3
echo "=== M33 RPMSG TEST ==="
modprobe imx_rpmsg_pingpong 2>&1 | head
# imx_rpmsg_pingpong prints "get N (src ...)" to the kernel console as the M33's
# pongs arrive; let the ~100-round exchange run, then idle.
sleep 20
echo "=== M33 RPMSG TEST DONE ==="
while true; do sleep 5; done
EOF
chmod +x "$TMP/myinit"
( cd "$TMP" && echo myinit | cpio -o -H newc 2>/dev/null > o.cpio )
cat "$BASE_INITRD" "$TMP/o.cpio" > "$TMP/c.cpio.gz"

# A55 kernel console + the M33 FreeRTOS console each to their own file (no
# mon:stdio - we tear the guest down by PID once the round-trip is observed).
"$QEMU" -M imx93-11x11-evk -audio driver=none -m 4G -display none \
    -kernel "$KERNEL" -dtb "$DTB" -initrd "$TMP/c.cpio.gz" \
    -append "console=ttyLP0,115200 cpuidle.off=1 rdinit=/myinit ignore_loglevel" \
    -device loader,file="$FW",addr=0x201E0000,force-raw=on \
    -device loader,file="$TMP/rsc.bin",addr=0x2021e000,force-raw=on \
    -serial "file:$A55LOG" -serial "file:$M33LOG" >/dev/null 2>&1 &
QPID=$!

# Wait for the M33 to report the exchange finished (or a hard cap). Kill by PID,
# never `pkill -f` (its pattern would match this script's own line and SIGKILL
# the shell).
for _ in $(seq 1 90); do
    grep -q 'Ping pong done' "$M33LOG" 2>/dev/null && break
    kill -0 "$QPID" 2>/dev/null || break        # guest died early
    sleep 1
done
kill "$QPID" 2>/dev/null; wait "$QPID" 2>/dev/null

# --- verdict: both endpoints must show the round-trip ---
fail=0
grep -q 'rpmsg-openamp-demo-channel' "$A55LOG" 2>/dev/null \
    || { echo "FAIL: A55 rpmsg channel never came up"; fail=1; }
rounds=$(grep -cE 'get [0-9]+ \(src' "$A55LOG" 2>/dev/null); rounds=${rounds:-0}
[ "$rounds" -ge "$MIN_ROUNDS" ] \
    || { echo "FAIL: only $rounds/$MIN_ROUNDS ping-pong replies reached Linux (doorbell relay not sustaining the round-trip)"; fail=1; }
grep -q 'Ping pong done' "$M33LOG" 2>/dev/null \
    || { echo "FAIL: M33 firmware never finished the exchange (no 'Ping pong done')"; fail=1; }

if [ "$fail" -eq 0 ]; then
    echo "PASS: A55<->M33 RPMsg ping-pong round-trip - $rounds replies to Linux, M33 finished"
    exit 0
fi
echo "--- A55 tail ---"; tail -12 "$A55LOG" 2>/dev/null
echo "--- M33 tail ---"; tail -6 "$M33LOG" 2>/dev/null
exit 1
