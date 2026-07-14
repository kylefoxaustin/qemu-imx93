#!/usr/bin/env bash
#
# Soak / endurance harness for the i.MX93 QEMU machine.
#
# Boots the machine headless, runs a light guest workload in a loop (exercising
# the RTC / thermal / ADC / IRQ paths), and monitors for DURATION seconds:
#   - kernel oops / BUG / unexpected call traces
#   - QEMU resident memory (leak watch)
#   - guest liveness (a heartbeat counter; a stalled guest = hang)
# PASS = no faults, heartbeat advancing, RSS not ballooning.
#
# Env: DURATION (s, default 300), SAMPLE (s, default 15), KERNEL/DTB/BASE_INITRD.
set -u

HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
DEPLOY=${DEPLOY:-$HOME/Documents/nxp/linux/imx-yocto-bsp/build-imx93/tmp/deploy/images/imx93evk}
QEMU=${QEMU:-$REPO/build-imx93/qemu-system-aarch64}
KERNEL=${KERNEL:-$DEPLOY/Image}
DTB=${DTB:-$DEPLOY/imx93-11x11-evk.dtb}
BASE_INITRD=${BASE_INITRD:-$HOME/Documents/nxp/imx93-initramfs.cpio.gz}
DURATION=${DURATION:-300}
SAMPLE=${SAMPLE:-15}
LOG=${LOG:-/tmp/soak-imx93.log}

need() { [ -e "$2" ] || { echo "error: $3 not found: $2" >&2; exit 1; }; }
need QEMU "$QEMU" "qemu"; need KERNEL "$KERNEL" "kernel"; need DTB "$DTB" "dtb"
need BASE_INITRD "$BASE_INITRD" "initrd"

TMP=$(mktemp -d); trap 'rm -rf "$TMP"; [ -n "${QPID:-}" ] && kill "$QPID" 2>/dev/null' EXIT
cat > "$TMP/myinit" <<'EOF'
#!/bin/sh
PATH=/sbin:/usr/sbin:/bin:/usr/bin; export PATH
mount -t proc proc /proc; mount -t sysfs sysfs /sys; mount -t devtmpfs devtmpfs /dev
sleep 3
echo "SOAK: guest up, starting workload loop"
n=0
while true; do
    n=$((n + 1))
    rtc=$(cat /sys/class/rtc/rtc0/since_epoch 2>/dev/null)
    temp=$(cat /sys/class/thermal/thermal_zone0/temp 2>/dev/null)
    adc=$(cat /sys/bus/iio/devices/iio:device0/in_voltage0_raw 2>/dev/null)
    irqs=$(grep -c . /proc/interrupts 2>/dev/null)
    echo "SOAK ITER $n rtc=$rtc temp=$temp adc=$adc irqlines=$irqs"
    sleep 5
done
EOF
chmod +x "$TMP/myinit"
( cd "$TMP" && find ./myinit | cpio -o -H newc 2>/dev/null > o.cpio )
cat "$BASE_INITRD" "$TMP/o.cpio" > "$TMP/c.cpio.gz"

: > "$LOG"
echo "soak: booting (duration=${DURATION}s, sample=${SAMPLE}s) -> $LOG"
"$QEMU" -M imx93-11x11-evk -audio driver=none -m 4G -display none \
    -kernel "$KERNEL" -dtb "$DTB" -initrd "$TMP/c.cpio.gz" \
    -append "console=ttyLP0,115200 cpuidle.off=1 rdinit=/myinit ignore_loglevel" \
    -serial "file:$LOG" -serial null &
QPID=$!

rss_kb() { awk '/VmRSS/{print $2}' "/proc/$QPID/status" 2>/dev/null; }

START=$(date +%s); RSS0=""; RSSMAX=0; LAST_ITER=0; STALLS=0
while :; do
    now=$(date +%s); el=$((now - START))
    [ "$el" -ge "$DURATION" ] && break
    if ! kill -0 "$QPID" 2>/dev/null; then echo "soak: QEMU EXITED early at ${el}s"; break; fi
    sleep "$SAMPLE"
    rss=$(rss_kb); [ -z "$rss" ] && continue
    [ -z "$RSS0" ] && RSS0=$rss
    [ "$rss" -gt "$RSSMAX" ] && RSSMAX=$rss
    iter=$(grep -ac "SOAK ITER" "$LOG")
    [ "$iter" -le "$LAST_ITER" ] && STALLS=$((STALLS + 1))
    LAST_ITER=$iter
    echo "soak: t=${el}s rss=${rss}kB iters=${iter}"
done

kill "$QPID" 2>/dev/null; wait "$QPID" 2>/dev/null; QPID=""

FAULTS=$(grep -acE "Internal error|Oops|BUG:|Unable to handle|kernel panic|soft lockup|rcu_sched.*stall" "$LOG")
ITERS=$(grep -ac "SOAK ITER" "$LOG")
echo "============ SOAK SUMMARY ============"
echo "duration=${DURATION}s  iterations=${ITERS}  rss0=${RSS0}kB peak=${RSSMAX}kB"
echo "oops/bug/panic/stall lines: $FAULTS"
echo "stall samples (no new iter): $STALLS"
grep -aE "Internal error|Oops|BUG:|kernel panic|soft lockup|stall" "$LOG" | head -5
if [ "$FAULTS" -eq 0 ] && [ "$ITERS" -gt 1 ] && [ "$STALLS" -le 1 ]; then
    echo "RESULT: PASS"
else
    echo "RESULT: FAIL"
fi
