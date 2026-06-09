#!/usr/bin/env bash
#
# Long-duration (default 36h) self-healing soak SUPERVISOR for the i.MX93 QEMU
# machine. Runs detached (setsid) so it outlives the launching shell/session.
#
# It loops in ~2h cycles until a persisted deadline. Each cycle:
#   MAIN  block  (imx93-11x11-evk.dtb)         - the endurance core: audio driven
#                CONTINUOUSLY (back-to-back /pcm_play, no gap) alongside i2c +
#                storage + network loops, for MAIN_BLOCK seconds.
#   CAMERA block (imx93-11x11-evk-mt9m114.dtb) - repeated V4L2 capture.
#   FLEXIO block (imx93-11x11-evk-flexio-i2c)  - repeated tmp105 I2C round-trip.
#
# Resilience: a guest oops/panic or an early QEMU exit does NOT abort the soak.
# The block's serial log is copied to incidents/, a counter is bumped, and the
# supervisor moves on (and, for an early exit, retries the same block once).
# The whole thing keeps going until the wall-clock deadline.
#
# Cumulative health (boots, incidents, total audio plays / captures / round
# trips, peak RSS) is written to $WORK/status after every block. A heartbeat
# timestamp ($WORK/heartbeat) is refreshed every monitor sample so an external
# watcher can tell "running" from "stuck".
#
# Env: TOTAL (s, default 129600=36h), MAIN_BLOCK (6600), CAM_BLOCK (300),
#      FLEXIO_BLOCK (300), MONSAMPLE (30). Paths as in soak-full.sh.
set -u

REPO=${REPO:-/home/kyle/Documents/GitHub/93emulator}
DEPLOY=${DEPLOY:-$HOME/Documents/nxp/linux/imx-yocto-bsp/build-imx93/tmp/deploy/images/imx93evk}
QEMU=${QEMU:-$REPO/build-imx93/qemu-system-aarch64}
KERNEL=${KERNEL:-$DEPLOY/Image}
DTB_MAIN=${DTB_MAIN:-$DEPLOY/imx93-11x11-evk.dtb}
DTB_CAM=${DTB_CAM:-$DEPLOY/imx93-11x11-evk-mt9m114.dtb}
DTB_FLEXIO=${DTB_FLEXIO:-$DEPLOY/imx93-11x11-evk-flexio-i2c.dtb}
BASE_INITRD=${BASE_INITRD:-$HOME/Documents/nxp/imx93-initramfs.cpio.gz}
PCM_PLAY=${PCM_PLAY:-/tmp/pcm_play}
V4L2_CAP=${V4L2_CAP:-/tmp/v4l2_cap}

WORK=${WORK:-/tmp/soak36}
TOTAL=${TOTAL:-129600}        # 36 hours
MAIN_BLOCK=${MAIN_BLOCK:-6600}
CAM_BLOCK=${CAM_BLOCK:-300}
FLEXIO_BLOCK=${FLEXIO_BLOCK:-300}
MONSAMPLE=${MONSAMPLE:-30}

FAULT_RE="Internal error|Oops|BUG:|Unable to handle|kernel panic|soft lockup|rcu_sched.*stall|rcu_preempt.*stall|watchdog: BUG"

mkdir -p "$WORK" "$WORK/incidents"
MASTER="$WORK/soak36.log"
QPID=""

ts()  { date '+%Y-%m-%d %H:%M:%S'; }
say() { echo "[$(ts)] $*" | tee -a "$MASTER"; }

# Refuse to run twice.
if [ -f "$WORK/supervisor.pid" ] && kill -0 "$(cat "$WORK/supervisor.pid" 2>/dev/null)" 2>/dev/null; then
    echo "supervisor already running (pid $(cat "$WORK/supervisor.pid")); abort" >&2
    exit 3
fi
echo $$ > "$WORK/supervisor.pid"

cleanup() {
    [ -n "$QPID" ] && kill "$QPID" 2>/dev/null
    # never leave an orphan boot behind
    pkill -f "imx93-11x11-evk.*$WORK" 2>/dev/null
    rm -f "$WORK/supervisor.pid"
}
trap cleanup EXIT INT TERM

# ----- persisted deadline (so a restart resumes to the ORIGINAL end time) -----
if [ -f "$WORK/deadline" ]; then
    DEADLINE=$(cat "$WORK/deadline")
    say "resuming; deadline already set ($(date -d "@$DEADLINE" '+%Y-%m-%d %H:%M:%S'))"
else
    DEADLINE=$(( $(date +%s) + TOTAL ))
    echo "$DEADLINE" > "$WORK/deadline"
    say "starting fresh; TOTAL=${TOTAL}s deadline=$(date -d "@$DEADLINE" '+%Y-%m-%d %H:%M:%S')"
fi

# ----- counters (persist across supervisor restarts) -------------------------
load() { [ -f "$WORK/$1" ] && cat "$WORK/$1" || echo 0; }
CYCLES=$(load c_cycles); BOOTS=$(load c_boots); INC=$(load c_incidents)
AUD=$(load c_audio); I2C=$(load c_i2c); STOR=$(load c_storage); NET=$(load c_net)
CAP=$(load c_captures); RT=$(load c_roundtrips); RSSPK=$(load c_rsspeak)
save() { echo "$2" > "$WORK/$1"; }

write_status() {
    local now; now=$(date +%s)
    cat > "$WORK/status.tmp" <<EOF
state=$1
phase=$2
updated=$(ts)
remaining_s=$(( DEADLINE - now ))
cycles=$CYCLES
boots=$BOOTS
incidents=$INC
audio_plays=$AUD
i2c_reads=$I2C
storage_ops=$STOR
net_pings=$NET
captures=$CAP
roundtrips=$RT
rss_peak_kb=$RSSPK
EOF
    mv "$WORK/status.tmp" "$WORK/status"
}

maxtok() {  # maxtok <key> <logfile>
    awk -v k="$1" '
        /SOAK ITER/ { for (i = 1; i <= NF; i++) {
            n = index($i, k "=")
            if (n == 1) { split($i, a, "="); if (a[2] + 0 > m) m = a[2] + 0 }
        } } END { print m + 0 }' "$2" 2>/dev/null
}

# ----- guest init scripts (generated once) -----------------------------------
gen_inits() {
cat > "$WORK/main.init" <<'EOF'
#!/bin/sh
PATH=/sbin:/usr/sbin:/bin:/usr/bin; export PATH
mount -t proc proc /proc 2>/dev/null
mount -t sysfs sysfs /sys 2>/dev/null
mount -t devtmpfs devtmpfs /dev 2>/dev/null
mkdir -p /tmp /mnt/sd
sleep 3
for m in snd-soc-fsl-utils snd-soc-fsl-sai imx-pcm-dma \
         snd-soc-wm8962 snd-soc-fsl-asoc-card \
         snd-soc-simple-card snd-soc-simple-card-utils; do
    modprobe "$m" 2>&1 | sed "s/^/  modprobe $m: /"
done
sleep 3
cat /proc/asound/cards 2>/dev/null
echo 0 > /tmp/acount; echo 0 > /tmp/icount; echo 0 > /tmp/scount; echo 0 > /tmp/ncount
# A) audio - CONTINUOUS, back-to-back, no gap.
( ac=0; while true; do
      if /pcm_play hw:1,0 1 >/tmp/aud.last 2>&1; then ac=$((ac + 1)); fi
      echo "$ac" > /tmp/acount
  done ) &
# B) i2c - hammer LPI2C1 reading the wm8962 id register (single transfer, no scan).
( ic=0; while true; do
      i2cget -y 0 0x1a 0x00 w >/dev/null 2>&1
      ic=$((ic + 1)); echo "$ic" > /tmp/icount; sleep 1
  done ) &
# C) storage - write/read/verify a payload on the SD card.
mount /dev/mmcblk0 /mnt/sd 2>/dev/null || \
    { mke2fs -q -t ext4 /dev/mmcblk0 2>/dev/null; mount /dev/mmcblk0 /mnt/sd 2>/dev/null; }
( sc=0; while true; do
      dd if=/dev/urandom of=/mnt/sd/blob bs=64k count=8 >/dev/null 2>&1; sync
      a=$(md5sum /mnt/sd/blob 2>/dev/null | cut -d' ' -f1)
      b=$(md5sum /mnt/sd/blob 2>/dev/null | cut -d' ' -f1)
      [ -n "$a" ] && [ "$a" = "$b" ] && sc=$((sc + 1))
      echo "$sc" > /tmp/scount; sleep 1
  done ) &
# D) network - ping the slirp gateway.
( nc=0; while true; do
      if ping -c1 -W2 10.0.2.2 >/dev/null 2>&1; then nc=$((nc + 1)); fi
      echo "$nc" > /tmp/ncount; sleep 2
  done ) &
n=0
while true; do
    n=$((n + 1))
    mf=$(awk '/MemFree/{print $2}' /proc/meminfo 2>/dev/null)
    il=$(grep -c . /proc/interrupts 2>/dev/null)
    echo "SOAK ITER $n memfree=${mf}kB irqlines=$il audio=$(cat /tmp/acount) i2c=$(cat /tmp/icount) storage=$(cat /tmp/scount) net=$(cat /tmp/ncount)"
    sleep 5
done
EOF

cat > "$WORK/cam.init" <<'EOF'
#!/bin/sh
PATH=/sbin:/usr/sbin:/bin:/usr/bin; export PATH
mount -t proc proc /proc 2>/dev/null
mount -t sysfs sysfs /sys 2>/dev/null
mount -t devtmpfs devtmpfs /dev 2>/dev/null
mkdir -p /tmp
sleep 4
ls -l /dev/video* /dev/media* 2>/dev/null
echo 0 > /tmp/ccount; echo 0 > /tmp/cfail
( cc=0; cf=0; while true; do
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

cat > "$WORK/flexio.init" <<'EOF'
#!/bin/sh
PATH=/sbin:/usr/sbin:/bin:/usr/bin; export PATH
mount -t proc proc /proc 2>/dev/null
mount -t sysfs sysfs /sys 2>/dev/null
mount -t devtmpfs devtmpfs /dev 2>/dev/null
mkdir -p /tmp
sleep 4
for d in /sys/bus/i2c/devices/i2c-*; do echo "  $d -> $(cat "$d/name" 2>/dev/null)"; done
echo 0 > /tmp/fcount; echo 0 > /tmp/ffail
( fc=0; ff=0; while true; do
      t=$(i2cget -y 8 0x49 0x00 w 2>/dev/null)
      i2cset -y 8 0x49 0x01 0x60 2>/dev/null
      r=$(i2cget -y 8 0x49 0x01 2>/dev/null)
      if [ -n "$t" ] && [ -n "$r" ]; then fc=$((fc + 1)); else ff=$((ff + 1)); fi
      echo "$fc" > /tmp/fcount; echo "$ff" > /tmp/ffail; sleep 1
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
chmod +x "$WORK"/*.init
}

mk_initrd() {  # mk_initrd <init> <out.cpio.gz> [bin ...]
    local initf=$1 out=$2; shift 2
    local d; d=$(mktemp -d)
    install -m755 "$initf" "$d/myinit"
    for b in "$@"; do [ -e "$b" ] && install -m755 "$b" "$d/$(basename "$b")"; done
    ( cd "$d" && find . -type f | cpio -o -H newc 2>/dev/null > "$WORK/o.cpio" )
    cat "$BASE_INITRD" "$WORK/o.cpio" > "$out"
    rm -rf "$d"
}

# ----- one block: boot, monitor, handle faults -------------------------------
# run_block <name> <dtb> <initrd> <dur> <extra qemu args...>; echoes "faulted" rc
run_block() {
    local name=$1 dtb=$2 initrd=$3 dur=$4; shift 4
    local log="$WORK/${name}.log"; : > "$log"
    BOOTS=$((BOOTS + 1)); save c_boots "$BOOTS"
    say "BLOCK $name start (dur=${dur}s, boot #$BOOTS, dtb=$(basename "$dtb"))"
    write_status running "$name"

    "$QEMU" -M imx93-11x11-evk -m 4G -display none -audio driver=none \
        -kernel "$KERNEL" -dtb "$dtb" -initrd "$initrd" "$@" \
        -append "console=ttyLP0,115200 cpuidle.off=1 rdinit=/myinit ignore_loglevel ip=dhcp" \
        -serial "file:$log" -serial null >/dev/null 2>&1 &
    QPID=$!

    local start now el rss faulted=0 reason=""
    start=$(date +%s)
    while :; do
        now=$(date +%s); el=$((now - start))
        date +%s > "$WORK/heartbeat"
        [ "$el" -ge "$dur" ] && break
        if ! kill -0 "$QPID" 2>/dev/null; then faulted=1; reason="qemu exited early at ${el}s"; break; fi
        if grep -aqE "$FAULT_RE" "$log"; then faulted=1; reason="kernel fault: $(grep -aoE "$FAULT_RE" "$log" | head -1)"; break; fi
        rss=$(awk '/VmRSS/{print $2}' "/proc/$QPID/status" 2>/dev/null)
        [ -n "$rss" ] && [ "$rss" -gt "$RSSPK" ] && { RSSPK=$rss; save c_rsspeak "$RSSPK"; }
        sleep "$MONSAMPLE"
    done

    kill "$QPID" 2>/dev/null; wait "$QPID" 2>/dev/null; QPID=""

    # roll the block's workload counters into the cumulative totals
    case "$name" in
        MAIN)
            AUD=$((AUD + $(maxtok audio "$log")));     save c_audio "$AUD"
            I2C=$((I2C + $(maxtok i2c "$log")));       save c_i2c "$I2C"
            STOR=$((STOR + $(maxtok storage "$log"))); save c_storage "$STOR"
            NET=$((NET + $(maxtok net "$log")));       save c_net "$NET" ;;
        CAMERA) CAP=$((CAP + $(maxtok captures "$log")));   save c_captures "$CAP" ;;
        FLEXIO) RT=$((RT + $(maxtok roundtrips "$log")));   save c_roundtrips "$RT" ;;
    esac

    if [ "$faulted" -eq 1 ]; then
        INC=$((INC + 1)); save c_incidents "$INC"
        local keep="$WORK/incidents/$(date +%Y%m%d-%H%M%S)-${name}.log"
        cp "$log" "$keep" 2>/dev/null
        say "INCIDENT #$INC in $name: $reason  (serial saved: $keep)"
        write_status incident "$name"
        return 1
    fi
    say "BLOCK $name ok (audio+=$(maxtok audio "$log") i2c+=$(maxtok i2c "$log") stor+=$(maxtok storage "$log") net+=$(maxtok net "$log") cap+=$(maxtok captures "$log") rt+=$(maxtok roundtrips "$log"))"
    write_status running "$name"
    return 0
}

# ----- setup ------------------------------------------------------------------
for f in "$QEMU" "$KERNEL" "$DTB_MAIN" "$DTB_CAM" "$DTB_FLEXIO" "$BASE_INITRD" "$PCM_PLAY" "$V4L2_CAP"; do
    [ -e "$f" ] || { say "FATAL: missing $f"; exit 2; }
done
# persistent SD backing image
SD_IMG="$WORK/sd.img"
if [ ! -e "$SD_IMG" ]; then
    dd if=/dev/zero of="$SD_IMG" bs=1M count=64 status=none
    mke2fs -q -t ext4 -b 1024 "$SD_IMG" 2>/dev/null || true
fi

gen_inits
say "building per-phase initramfs images..."
mk_initrd "$WORK/main.init"   "$WORK/main.cpio.gz"   "$PCM_PLAY"
mk_initrd "$WORK/cam.init"    "$WORK/cam.cpio.gz"    "$V4L2_CAP"
mk_initrd "$WORK/flexio.init" "$WORK/flexio.cpio.gz"
say "initramfs ready; entering cycle loop"

# ----- the 36h cycle loop -----------------------------------------------------
remaining() { echo $(( DEADLINE - $(date +%s) )); }
cap_dur() { local want=$1 rem; rem=$(remaining); [ "$rem" -lt "$want" ] && echo "$rem" || echo "$want"; }

while [ "$(remaining)" -gt 60 ]; do
    CYCLES=$((CYCLES + 1)); save c_cycles "$CYCLES"
    say "===== CYCLE $CYCLES (remaining $(( $(remaining) / 60 )) min) ====="

    # MAIN endurance block (retry once on an early-exit fault).
    d=$(cap_dur "$MAIN_BLOCK")
    if [ "$d" -gt 60 ]; then
        run_block MAIN "$DTB_MAIN" "$WORK/main.cpio.gz" "$d" \
            -drive "if=sd,file=$SD_IMG,format=raw" -nic user || {
            say "MAIN faulted; retrying once"
            d=$(cap_dur "$MAIN_BLOCK")
            [ "$d" -gt 60 ] && run_block MAIN "$DTB_MAIN" "$WORK/main.cpio.gz" "$d" \
                -drive "if=sd,file=$SD_IMG,format=raw" -nic user || true
        }
    fi

    d=$(cap_dur "$CAM_BLOCK")
    [ "$d" -gt 30 ] && { run_block CAMERA "$DTB_CAM" "$WORK/cam.cpio.gz" "$d" || true; }

    d=$(cap_dur "$FLEXIO_BLOCK")
    [ "$d" -gt 30 ] && { run_block FLEXIO "$DTB_FLEXIO" "$WORK/flexio.cpio.gz" "$d" \
        -device tmp105,bus=flexio1-i2c,address=0x49 || true; }
done

# ----- done -------------------------------------------------------------------
write_status done none
say "================ SOAK36 COMPLETE ================"
say "cycles=$CYCLES boots=$BOOTS incidents=$INC"
say "audio_plays=$AUD i2c_reads=$I2C storage_ops=$STOR net_pings=$NET captures=$CAP roundtrips=$RT"
say "peak QEMU RSS=${RSSPK}kB"
if [ "$INC" -eq 0 ]; then say "RESULT: PASS (no faults over the full duration)"
else say "RESULT: COMPLETED WITH $INC INCIDENT(S) - see $WORK/incidents/"; fi
rm -f "$WORK/deadline"   # clean slate for any future run
