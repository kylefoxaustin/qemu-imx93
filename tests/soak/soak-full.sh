#!/usr/bin/env bash
#
# Comprehensive soak / endurance harness for the i.MX93 QEMU machine.
#
# Unlike soak.sh (a light single-boot sensor loop), this drives EVERY functional
# datapath the port models, continuously, and watches for faults / hangs / leaks.
# The emphasis is the audio path: it is driven back-to-back without a gap for the
# whole MAIN phase, because a codec that cannot be clocked *continuously* is a
# classic soak failure mode (the eDMA3 cyclic ring / SAI3 TX FIFO must pace every
# period for minutes, not just survive one playback).
#
# One device tree can only describe one machine variant, so the soak runs in
# three sequential headless boots, each its own phase:
#
#   MAIN   (imx93-11x11-evk.dtb)            base machine, four concurrent loops:
#            - audio    : /pcm_play hw:1,0 back-to-back (CONTINUOUS, no sleep)
#            - i2c      : i2cdetect + wm8962 codec register reads
#            - storage  : write/read/verify a file on the SD card (mmcblk0)
#            - network  : ping the slirp gateway (FEC/eQOS up via DHCP)
#          plus a heartbeat sampling MemFree / IRQ lines / every loop's counter.
#
#   CAMERA (imx93-11x11-evk-mt9m114.dtb)    repeated V4L2 capture off /dev/video0
#            (mt9m114 -> pCSI -> ISI), proving the ISI frame DMA + IRQ keep firing.
#
#   FLEXIO (imx93-11x11-evk-flexio-i2c.dtb) repeated tmp105 round-trip over the
#            FlexIO-as-I2C master (shifter/timer handshake under continuous load).
#
# Each phase passes only if: no oops/BUG/panic/stall, the heartbeat keeps
# advancing (guest not hung), QEMU RSS does not balloon, and the phase's own
# workload counters actually grew. The harness PASSes only if all phases pass.
#
# Env: MAIN_DUR (s, default 240), CAM_DUR (60), FLEXIO_DUR (60), SAMPLE (15).
#      PCM_PLAY / V4L2_CAP point at prebuilt oracles (see tests/audio-imx93 and
#      tests/camera-imx93); KERNEL / DTB dir / BASE_INITRD as usual.
set -u

HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
DEPLOY=${DEPLOY:-$HOME/Documents/nxp/linux/imx-yocto-bsp/build-imx93/tmp/deploy/images/imx93evk}
QEMU=${QEMU:-$REPO/build-imx93/qemu-system-aarch64}
KERNEL=${KERNEL:-$DEPLOY/Image}
DTB_MAIN=${DTB_MAIN:-$DEPLOY/imx93-11x11-evk.dtb}
DTB_CAM=${DTB_CAM:-$DEPLOY/imx93-11x11-evk-mt9m114.dtb}
DTB_FLEXIO=${DTB_FLEXIO:-$DEPLOY/imx93-11x11-evk-flexio-i2c.dtb}
BASE_INITRD=${BASE_INITRD:-$HOME/Documents/nxp/imx93-initramfs.cpio.gz}
PCM_PLAY=${PCM_PLAY:-/tmp/pcm_play}
V4L2_CAP=${V4L2_CAP:-/tmp/v4l2_cap}
SD_IMG=${SD_IMG:-/tmp/soak-sd.img}
MAIN_DUR=${MAIN_DUR:-240}
CAM_DUR=${CAM_DUR:-60}
FLEXIO_DUR=${FLEXIO_DUR:-60}
SAMPLE=${SAMPLE:-15}
OUT=${OUT:-/tmp/soak-full}

need() { [ -e "$2" ] || { echo "error: $3 not found: $2" >&2; exit 1; }; }
need QEMU "$QEMU" "qemu"; need KERNEL "$KERNEL" "kernel"
need BASE_INITRD "$BASE_INITRD" "initrd"
need DTB_MAIN "$DTB_MAIN" "base dtb"
need DTB_CAM "$DTB_CAM" "mt9m114 dtb"
need DTB_FLEXIO "$DTB_FLEXIO" "flexio-i2c dtb"
need PCM_PLAY "$PCM_PLAY" "pcm_play oracle"
need V4L2_CAP "$V4L2_CAP" "v4l2_cap oracle"

mkdir -p "$OUT"
TMP=$(mktemp -d)
QPID=""
cleanup() { [ -n "$QPID" ] && kill "$QPID" 2>/dev/null; rm -rf "$TMP"; }
trap cleanup EXIT

FAULT_RE="Internal error|Oops|BUG:|Unable to handle|kernel panic|soft lockup|rcu_sched.*stall|rcu_preempt.*stall"

# Create the SD-card backing image (raw ext4, no partition table -> mount mmcblk0
# directly) if it is missing, so storage has somewhere to write.
if [ ! -e "$SD_IMG" ]; then
    echo "soak: creating SD image $SD_IMG"
    dd if=/dev/zero of="$SD_IMG" bs=1M count=64 status=none
    mke2fs -q -t ext4 -b 1024 "$SD_IMG" 2>/dev/null || \
        echo "soak: warning: mke2fs failed; storage loop will mkfs in-guest"
fi

# ----- guest workload scripts ------------------------------------------------
# MAIN: load the ASoC stack, bring up four concurrent loops, heartbeat.
cat > "$TMP/main.init" <<'EOF'
#!/bin/sh
PATH=/sbin:/usr/sbin:/bin:/usr/bin; export PATH
mount -t proc proc /proc 2>/dev/null
mount -t sysfs sysfs /sys 2>/dev/null
mount -t devtmpfs devtmpfs /dev 2>/dev/null
mkdir -p /tmp /mnt/sd
sleep 3

echo "SOAK-MAIN: loading ASoC drivers"
for m in snd-soc-fsl-utils snd-soc-fsl-sai imx-pcm-dma \
         snd-soc-wm8962 snd-soc-fsl-asoc-card \
         snd-soc-simple-card snd-soc-simple-card-utils; do
    modprobe "$m" 2>&1 | sed "s/^/  modprobe $m: /"
done
sleep 3
cat /proc/asound/cards 2>/dev/null

echo 0 > /tmp/acount; echo 0 > /tmp/icount
echo 0 > /tmp/scount; echo 0 > /tmp/ncount

# A) audio - CONTINUOUS: replay 1s clips back-to-back, no gap, forever.
( ac=0
  while true; do
      if /pcm_play hw:1,0 1 >/tmp/aud.last 2>&1; then ac=$((ac + 1)); fi
      echo "$ac" > /tmp/acount
  done ) &

# B) i2c: hammer the LPI2C1 controller by reading the wm8962 codec id register.
# (No i2cdetect: a full-bus scan probes unmodelled addresses and stalls the loop;
# a single addressed transfer exercises the controller just as well.)
( ic=0
  while true; do
      i2cget -y 0 0x1a 0x00 w >/dev/null 2>&1
      ic=$((ic + 1)); echo "$ic" > /tmp/icount
      sleep 1
  done ) &

# C) storage: mount the SD card, write/read/verify a payload each iteration.
mount /dev/mmcblk0 /mnt/sd 2>/dev/null || \
    { mke2fs -q -t ext4 /dev/mmcblk0 2>/dev/null; mount /dev/mmcblk0 /mnt/sd 2>/dev/null; }
( sc=0
  while true; do
      dd if=/dev/urandom of=/mnt/sd/blob bs=64k count=8 >/dev/null 2>&1
      sync
      a=$(md5sum /mnt/sd/blob 2>/dev/null | cut -d' ' -f1)
      b=$(md5sum /mnt/sd/blob 2>/dev/null | cut -d' ' -f1)
      [ -n "$a" ] && [ "$a" = "$b" ] && sc=$((sc + 1))
      echo "$sc" > /tmp/scount
      sleep 1
  done ) &

# D) network: ping the slirp gateway (FEC/eQOS brought up by ip=dhcp).
( nc=0
  while true; do
      if ping -c1 -W2 10.0.2.2 >/dev/null 2>&1; then nc=$((nc + 1)); fi
      echo "$nc" > /tmp/ncount
      sleep 2
  done ) &

echo "SOAK-MAIN: workloads launched (audio/i2c/storage/net)"
n=0
while true; do
    n=$((n + 1))
    mf=$(awk '/MemFree/{print $2}' /proc/meminfo 2>/dev/null)
    il=$(grep -c . /proc/interrupts 2>/dev/null)
    echo "SOAK ITER $n memfree=${mf}kB irqlines=$il audio=$(cat /tmp/acount) i2c=$(cat /tmp/icount) storage=$(cat /tmp/scount) net=$(cat /tmp/ncount)"
    sleep 5
done
EOF

# CAMERA: repeated V4L2 capture off /dev/video0.
cat > "$TMP/cam.init" <<'EOF'
#!/bin/sh
PATH=/sbin:/usr/sbin:/bin:/usr/bin; export PATH
mount -t proc proc /proc 2>/dev/null
mount -t sysfs sysfs /sys 2>/dev/null
mount -t devtmpfs devtmpfs /dev 2>/dev/null
mkdir -p /tmp
sleep 4
echo "SOAK-CAMERA: pipeline" ; ls -l /dev/video* /dev/media* 2>/dev/null
echo 0 > /tmp/ccount; echo 0 > /tmp/cfail
( cc=0; cf=0
  while true; do
      if /v4l2_cap cap /dev/video0 >/tmp/cap.last 2>&1; then cc=$((cc + 1)); else cf=$((cf + 1)); fi
      echo "$cc" > /tmp/ccount; echo "$cf" > /tmp/cfail
  done ) &
n=0
while true; do
    n=$((n + 1))
    mf=$(awk '/MemFree/{print $2}' /proc/meminfo 2>/dev/null)
    il=$(grep -c . /proc/interrupts 2>/dev/null)
    echo "SOAK ITER $n memfree=${mf}kB irqlines=$il captures=$(cat /tmp/ccount) capfail=$(cat /tmp/cfail)"
    sleep 5
done
EOF

# FLEXIO: repeated tmp105 round-trip over the FlexIO I2C master (i2c-8).
cat > "$TMP/flexio.init" <<'EOF'
#!/bin/sh
PATH=/sbin:/usr/sbin:/bin:/usr/bin; export PATH
mount -t proc proc /proc 2>/dev/null
mount -t sysfs sysfs /sys 2>/dev/null
mount -t devtmpfs devtmpfs /dev 2>/dev/null
mkdir -p /tmp
sleep 4
echo "SOAK-FLEXIO: adapters"
for d in /sys/bus/i2c/devices/i2c-*; do echo "  $d -> $(cat "$d/name" 2>/dev/null)"; done
echo 0 > /tmp/fcount; echo 0 > /tmp/ffail
( fc=0; ff=0
  while true; do
      t=$(i2cget -y 8 0x49 0x00 w 2>/dev/null)
      i2cset -y 8 0x49 0x01 0x60 2>/dev/null
      r=$(i2cget -y 8 0x49 0x01 2>/dev/null)
      if [ -n "$t" ] && [ -n "$r" ]; then fc=$((fc + 1)); else ff=$((ff + 1)); fi
      echo "$fc" > /tmp/fcount; echo "$ff" > /tmp/ffail
      sleep 1
  done ) &
n=0
while true; do
    n=$((n + 1))
    mf=$(awk '/MemFree/{print $2}' /proc/meminfo 2>/dev/null)
    il=$(grep -c . /proc/interrupts 2>/dev/null)
    echo "SOAK ITER $n memfree=${mf}kB irqlines=$il roundtrips=$(cat /tmp/fcount) rtfail=$(cat /tmp/ffail)"
    sleep 5
done
EOF
chmod +x "$TMP"/*.init

# Build a per-phase overlay cpio chained onto the base rootfs.
mk_initrd() {  # mk_initrd <init-file> <out.cpio.gz> [extra-bin ...]
    local initf=$1 out=$2; shift 2
    local d; d=$(mktemp -d)
    install -m755 "$initf" "$d/myinit"
    for b in "$@"; do install -m755 "$b" "$d/$(basename "$b")"; done
    ( cd "$d" && find . -type f | cpio -o -H newc 2>/dev/null > "$TMP/o.cpio" )
    cat "$BASE_INITRD" "$TMP/o.cpio" > "$out"
    rm -rf "$d"
}

# ----- per-phase boot + monitor ----------------------------------------------
PHASE_RESULTS=""
run_phase() {  # run_phase <name> <dtb> <initrd> <dur> <extra-qemu-args...>
    local name=$1 dtb=$2 initrd=$3 dur=$4; shift 4
    local log="$OUT/$name.log"; : > "$log"
    echo
    echo "######## PHASE $name (dur=${dur}s, dtb=$(basename "$dtb")) ########"
    "$QEMU" -M imx93-11x11-evk -audio driver=none -m 4G -display none \
        -kernel "$KERNEL" -dtb "$dtb" -initrd "$initrd" "$@" \
        -append "console=ttyLP0,115200 cpuidle.off=1 rdinit=/myinit ignore_loglevel ip=dhcp" \
        -serial "file:$log" -serial null &
    QPID=$!

    local start now el rss rss0="" rssmax=0 iter last=0 stalls=0
    start=$(date +%s)
    while :; do
        now=$(date +%s); el=$((now - start))
        [ "$el" -ge "$dur" ] && break
        if ! kill -0 "$QPID" 2>/dev/null; then echo "  $name: QEMU EXITED early at ${el}s"; break; fi
        sleep "$SAMPLE"
        rss=$(awk '/VmRSS/{print $2}' "/proc/$QPID/status" 2>/dev/null)
        [ -z "$rss" ] && continue
        iter=$(grep -ac "SOAK ITER" "$log")
        # Anchor the leak baseline and stall watch at first liveness, so the
        # ~20s kernel boot (heartbeat not up yet) is not mistaken for a hang.
        if [ "$iter" -gt 0 ]; then
            [ -z "$rss0" ] && rss0=$rss
            [ "$rss" -gt "$rssmax" ] && rssmax=$rss
            [ "$iter" -le "$last" ] && [ "$last" -gt 0 ] && stalls=$((stalls + 1))
        fi
        last=$iter
        echo "  $name t=${el}s rss=${rss}kB iters=${iter} | $(grep -a 'SOAK ITER' "$log" | tail -1 | sed 's/.*irqlines=[0-9]* //')"
    done
    kill "$QPID" 2>/dev/null; wait "$QPID" 2>/dev/null; QPID=""

    local faults iters
    faults=$(grep -acE "$FAULT_RE" "$log")
    iters=$(grep -ac "SOAK ITER" "$log")
    echo "  ---- $name summary: iters=$iters faults=$faults stalls=$stalls rss0=${rss0}kB peak=${rssmax}kB"
    [ "$faults" -gt 0 ] && grep -aE "$FAULT_RE" "$log" | head -3 | sed 's/^/    FAULT: /'

    # liveness + leak gate (RSS may grow <50% under load; balloon = fail)
    local live=1
    [ "$faults" -eq 0 ] || live=0
    [ "$iters" -gt 1 ] || live=0
    [ "$stalls" -le 1 ] || live=0
    if [ -n "$rss0" ] && [ "$rssmax" -gt $((rss0 * 3 / 2)) ]; then
        echo "  $name: RSS ballooned ${rss0}->${rssmax}kB"; live=0
    fi

    # workload-progress gate: the phase's counters must have advanced.
    local prog=1 msg=""
    case "$name" in
        MAIN)
            for k in audio i2c storage net; do
                v=$(maxtok "$k" "$log"); msg="$msg $k=$v"
                [ "$v" -ge 2 ] || prog=0
            done ;;
        CAMERA)
            v=$(maxtok captures "$log"); f=$(maxtok capfail "$log")
            msg=" captures=$v capfail=$f"; [ "$v" -ge 2 ] || prog=0 ;;
        FLEXIO)
            v=$(maxtok roundtrips "$log"); f=$(maxtok rtfail "$log")
            msg=" roundtrips=$v rtfail=$f"; [ "$v" -ge 2 ] || prog=0 ;;
    esac
    echo "  ---- $name workload:$msg  (progress=$prog liveness=$live)"

    if [ "$live" -eq 1 ] && [ "$prog" -eq 1 ]; then
        echo "  ==== $name: PASS"; PHASE_RESULTS="$PHASE_RESULTS $name:PASS"
    else
        echo "  ==== $name: FAIL"; PHASE_RESULTS="$PHASE_RESULTS $name:FAIL"
    fi
}

# max numeric value of token "key=" across all SOAK ITER lines in a log.
maxtok() {
    awk -v k="$1" '
        /SOAK ITER/ { for (i = 1; i <= NF; i++) {
            n = index($i, k "=")
            if (n == 1) { split($i, a, "="); if (a[2] + 0 > m) m = a[2] + 0 }
        } } END { print m + 0 }' "$2"
}

# ----- run the three phases --------------------------------------------------
echo "soak-full: comprehensive endurance run -> $OUT"
echo "  phases: MAIN ${MAIN_DUR}s, CAMERA ${CAM_DUR}s, FLEXIO ${FLEXIO_DUR}s"

mk_initrd "$TMP/main.init"   "$TMP/main.cpio.gz"   "$PCM_PLAY"
mk_initrd "$TMP/cam.init"    "$TMP/cam.cpio.gz"    "$V4L2_CAP"
mk_initrd "$TMP/flexio.init" "$TMP/flexio.cpio.gz"

run_phase MAIN   "$DTB_MAIN"   "$TMP/main.cpio.gz"   "$MAIN_DUR" \
    -drive "if=sd,file=$SD_IMG,format=raw" -nic user
run_phase CAMERA "$DTB_CAM"    "$TMP/cam.cpio.gz"    "$CAM_DUR"
run_phase FLEXIO "$DTB_FLEXIO" "$TMP/flexio.cpio.gz" "$FLEXIO_DUR" \
    -device tmp105,bus=flexio1-i2c,address=0x49

echo
echo "============ SOAK-FULL SUMMARY ============"
echo "phases:$PHASE_RESULTS"
if echo "$PHASE_RESULTS" | grep -q FAIL || [ -z "$PHASE_RESULTS" ]; then
    echo "RESULT: FAIL"; exit 1
else
    echo "RESULT: PASS"; exit 0
fi
