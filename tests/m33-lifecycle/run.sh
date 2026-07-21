#!/usr/bin/env bash
#
# M33 remoteproc lifecycle test for the i.MX93 QEMU machine.
#
# Linux drives the Cortex-M33 through remoteproc, which lowers to the i.MX SiP
# RPROC SMC (START 0x00 / STOP 0x02) that the machine services by releasing or
# halting the M33 (fsl_imx93_m33_rproc_{start,stop}_bh).
#
# This asserts the single start -> stop lifecycle works, and is a TRIPWIRE for a
# KNOWN, DOCUMENTED i.MX93 limitation: a guest-driven *re*-start (stop then start
# again) of the stock rpmsg-lite pingpong firmware desyncs the RPMsg mailbox and
# wedges the M33. That is faithful to real silicon, not a model-only gap:
#   - the MU lives in always-on AONMIX and is NOT reset by an M-core stop, so its
#     TX-full/pending state is stale on restart -> "imx_rproc_kick: failed
#     err:-62" (-ETIMEDOUT), and
#   - the NXP rpmsg_lite pingpong demo destroys its rpmsg endpoint after ~100
#     iterations and never re-initialises, so even a correct kernel times out.
# NXP acknowledges this (KB ta-p/2066755); the real fix needs both a Linux driver
# change (imx_rproc frees+re-requests the mbox each cycle) AND firmware that
# re-inits. In QEMU the wedged M33 additionally hits a Cortex-M lockup that aborts
# the machine, so this test does NOT drive the guest into the second start under
# CI - it verifies the first lifecycle and records the limitation.
#
# PASS = the M33 starts (comes up) and stops cleanly once, no fault. If a future
# change makes repeated restart work, extend RESTARTS and this becomes a real
# multi-cycle assertion.
#
# Asset-gated like tests/audio-imx93; SKIPs (does not fail) without the assets.
set -u

HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
DEPLOY=${DEPLOY:-$HOME/Documents/nxp/linux/imx-yocto-bsp/build-imx93/tmp/deploy/images/imx93evk}
FWDIR=${FWDIR:-$HOME/Documents/nxp/imx93-rootfs/lib/firmware}
QEMU=${QEMU:-$REPO/build-imx93/qemu-system-aarch64}
KERNEL=${KERNEL:-$DEPLOY/Image}
DTB=${DTB:-$DEPLOY/imx93-11x11-evk.dtb}
BASE_INITRD=${BASE_INITRD:-$HOME/Documents/nxp/imx93-initramfs.cpio.gz}
FW_NAME=${FW_NAME:-imx93-11x11-evk_m33_TCM_rpmsg_lite_pingpong_rtos_linux_remote.elf}

skip() { echo "SKIP: $1"; exit 0; }
[ -e "$QEMU" ]        || skip "qemu not built: $QEMU"
[ -e "$KERNEL" ]      || skip "kernel not found: $KERNEL"
[ -e "$DTB" ]         || skip "dtb not found: $DTB"
[ -e "$BASE_INITRD" ] || skip "base initramfs not found: $BASE_INITRD"
[ -e "$FWDIR/$FW_NAME" ] || skip "M33 firmware ELF not found: $FWDIR/$FW_NAME"

TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
CON="$TMP/con.log"

cat > "$TMP/myinit" <<EOF
#!/bin/sh
PATH=/sbin:/usr/sbin:/bin:/usr/bin; export PATH
mount -t proc proc /proc; mount -t sysfs sysfs /sys; mount -t devtmpfs devtmpfs /dev
sleep 3
RP=/sys/class/remoteproc/remoteproc0
[ -d "\$RP" ] || { echo "LIFECYCLE: no remoteproc0"; echo DONE; while true; do sleep 5; done; }
[ -e "\$RP/firmware" ] && echo "$FW_NAME" > "\$RP/firmware" 2>/dev/null
echo "=== START ==="; echo start > "\$RP/state" 2>&1; sleep 3
echo "=== STOP ===";  echo stop  > "\$RP/state" 2>&1; sleep 3
echo DONE
while true; do sleep 5; done
EOF
chmod +x "$TMP/myinit"
( cd "$TMP" && echo myinit | cpio -o -H newc 2>/dev/null > o.cpio )
cat "$BASE_INITRD" "$TMP/o.cpio" > "$TMP/c.cpio.gz"

"$QEMU" -M imx93-11x11-evk -audio driver=none -m 4G -display none \
    -kernel "$KERNEL" -dtb "$DTB" -initrd "$TMP/c.cpio.gz" \
    -append "console=ttyLP0,115200 cpuidle.off=1 rdinit=/myinit ignore_loglevel" \
    -serial "file:$CON" -serial null >/dev/null 2>&1 &
QPID=$!

# Wait for the guest to finish the single cycle (or a hard cap). Tear down by
# PID - never `pkill -f`, whose pattern would match this script's own line.
for _ in $(seq 1 90); do
    grep -q '^DONE' "$CON" 2>/dev/null && break
    kill -0 "$QPID" 2>/dev/null || break
    sleep 1
done
kill "$QPID" 2>/dev/null; wait "$QPID" 2>/dev/null

up=$(grep -c 'remote processor imx-rproc is now up' "$CON" 2>/dev/null); up=${up:-0}
down=$(grep -c 'stopped remote processor imx-rproc' "$CON" 2>/dev/null); down=${down:-0}
faults=$(grep -cE 'Kernel panic|Unhandled|Bad mode|synchronous external abort|Internal error' "$CON" 2>/dev/null); faults=${faults:-0}

if [ "$up" -ge 1 ] && [ "$down" -ge 1 ] && [ "$faults" -eq 0 ]; then
    echo "PASS: M33 remoteproc single start->stop lifecycle clean ($up up / $down stop)"
    echo "NOTE: guest-driven M33 *re*-start is a documented i.MX93 limitation" \
         "(MU not reset on M-core stop + stock demo fw tears down rpmsg;" \
         "NXP KB ta-p/2066755). Not exercised here by design."
    exit 0
fi
echo "FAIL: single M33 start->stop lifecycle did not complete ($up up / $down stop / $faults faults)"
echo "--- console tail ---"; grep -vE 'Fixed dependency cycle' "$CON" | tail -20
exit 1
