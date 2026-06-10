#!/usr/bin/env bash
#
# Long-duration (default 36h) self-healing soak SUPERVISOR for the i.MX93 QEMU
# machine. Runs detached (setsid) so it outlives the launching shell/session.
#
# It loops in ~2h cycles until a persisted deadline. Each cycle:
#   MAIN   block (imx93-11x11-evk.dtb)         - the endurance core: the WHOLE
#                audio surface driven CONTINUOUSLY, back-to-back, no gap -
#                wm8962/SAI3 playback AND capture (SAI-RX), XCVR/SPDIF playback,
#                and MICFIL PDM capture, all at once (every eDMA channel busy) -
#                alongside i2c + storage + network loops, for MAIN_BLOCK seconds.
#   CAMERA block (imx93-11x11-evk-mt9m114.dtb) - repeated V4L2 capture (parallel
#                CSI path: mt9m114 -> pcsi -> ISI).
#   MIPICAM block(imx93-11x11-frdm-ov5640.dtb) - repeated V4L2 capture (MIPI
#                CSI-2 path: ov5640 -> dw-mipi-csi2 -> ISI).
#   DISPLAY block(imx93-11x11-evk-rm67199.dtb) - LCDIFv3 -> DSI -> rm67199 panel
#                scanout: fill /dev/fb0 and let the page-flip loop DMA it out.
#   FLEXIO block (imx93-11x11-evk-flexio-i2c)  - repeated tmp105 I2C round-trip
#                (also exercises the FlexIO defer-shift anti-storm fix).
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
#      MIPI_BLOCK (300), DISP_BLOCK (300), FLEXIO_BLOCK (300), MONSAMPLE (30).
#      Paths as in soak-full.sh.
set -u

REPO=${REPO:-/home/kyle/Documents/GitHub/93emulator}
DEPLOY=${DEPLOY:-$HOME/Documents/nxp/linux/imx-yocto-bsp/build-imx93/tmp/deploy/images/imx93evk}
QEMU=${QEMU:-$REPO/build-imx93/qemu-system-aarch64}
KERNEL=${KERNEL:-$DEPLOY/Image}
DTB_MAIN=${DTB_MAIN:-$DEPLOY/imx93-11x11-evk.dtb}
DTB_CAM=${DTB_CAM:-$DEPLOY/imx93-11x11-evk-mt9m114.dtb}
DTB_MIPI=${DTB_MIPI:-$DEPLOY/imx93-11x11-frdm-ov5640.dtb}
DTB_DISP=${DTB_DISP:-$DEPLOY/imx93-11x11-evk-rm67199.dtb}
DTB_FLEXIO=${DTB_FLEXIO:-$DEPLOY/imx93-11x11-evk-flexio-i2c.dtb}
BASE_INITRD=${BASE_INITRD:-$HOME/Documents/nxp/imx93-initramfs.cpio.gz}
PCM_PLAY=${PCM_PLAY:-/tmp/pcm_play}
PCM_CAP=${PCM_CAP:-/tmp/pcm_capture}
V4L2_CAP=${V4L2_CAP:-/tmp/v4l2_cap}

WORK=${WORK:-/tmp/soak36}
TOTAL=${TOTAL:-129600}        # 36 hours
MAIN_BLOCK=${MAIN_BLOCK:-6600}
CAM_BLOCK=${CAM_BLOCK:-300}
MIPI_BLOCK=${MIPI_BLOCK:-300}
DISP_BLOCK=${DISP_BLOCK:-300}
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
RX=$(load c_sairx); SPD=$(load c_spdif); MIC=$(load c_micfil)
CAP=$(load c_captures); MIPI=$(load c_mipicap); FLIP=$(load c_flips)
RT=$(load c_roundtrips); RSSPK=$(load c_rsspeak)
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
sai_rx_captures=$RX
spdif_plays=$SPD
micfil_captures=$MIC
i2c_reads=$I2C
storage_ops=$STOR
net_pings=$NET
captures=$CAP
mipi_captures=$MIPI
panel_flips=$FLIP
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
# Full audio surface: SAI/wm8962 (snd-soc-wm8962 + fsl-asoc-card), XCVR/SPDIF
# (snd-soc-fsl-xcvr + imx-card), MICFIL PDM (snd-soc-fsl-micfil + imx-card +
# the dmic codec).
for m in snd-soc-fsl-utils snd-soc-fsl-sai imx-pcm-dma \
         snd-soc-wm8962 snd-soc-fsl-asoc-card \
         snd-soc-fsl-micfil snd-soc-fsl-xcvr snd-soc-imx-card snd-soc-dmic \
         snd-soc-simple-card snd-soc-simple-card-utils; do
    modprobe "$m" 2>&1 | sed "s/^/  modprobe $m: /"
done
sleep 4
cat /proc/asound/cards 2>/dev/null
# Resolve card numbers by id (don't hardcode - enumeration order can shift).
card_of() { for c in 0 1 2 3 4 5; do [ -e /proc/asound/card$c/id ] || continue
    case "$(cat /proc/asound/card$c/id)" in *$1*) echo "$c"; return;; esac; done; }
WM=$(card_of wm8962); XC=$(card_of xcvr); MF=$(card_of micfil)
echo "AUDIO CARDS wm8962=$WM xcvr=$XC micfil=$MF"
echo 0 > /tmp/acount; echo 0 > /tmp/rxcount; echo 0 > /tmp/spcount; echo 0 > /tmp/mfcount
echo 0 > /tmp/icount; echo 0 > /tmp/scount; echo 0 > /tmp/ncount
# Audio datapath note: SAI3 and XCVR share eDMA2's single dma-req line, and the
# model advances the first cyclic channel per request pulse - so one eDMA2
# stream gets full service and any second eDMA2 stream is starved (a known eDMA
# model limitation; see the soak README). MICFIL is on eDMA1 (independent). So
# we run exactly ONE eDMA2 audio stream at a time at full bandwidth, ROTATING
# SAI3 playback -> SAI3 capture (SAI-RX) -> XCVR/SPDIF playback, while MICFIL
# PDM capture runs CONTINUOUSLY on eDMA1 throughout. Audio never stops (one
# eDMA2 path + MICFIL are always live) and every datapath is hammered in turn.
AUDIO_OPS=20   # back-to-back ops per eDMA2 phase before rotating (~2-3s each)
inc() { echo $(( $(cat "$1") + 1 )) > "$1"; }
# MICFIL PDM capture - CONTINUOUS (eDMA1, independent of the eDMA2 rotation).
[ -n "$MF" ] && ( while true; do
      /pcm_capture plughw:$MF,0 1 >/tmp/micfil.last 2>&1 && inc /tmp/mfcount
  done ) &
# Rotating single eDMA2 stream: SAI-TX play -> SAI-RX capture -> XCVR/SPDIF.
( while true; do
      i=0; while [ $i -lt $AUDIO_OPS ]; do
          [ -n "$WM" ] && /pcm_play hw:$WM,0 1 >/tmp/aud.last 2>&1 && inc /tmp/acount
          i=$((i + 1)); done
      i=0; while [ $i -lt $AUDIO_OPS ]; do
          [ -n "$WM" ] && /pcm_capture hw:$WM,0 1 >/tmp/rx.last 2>&1 && inc /tmp/rxcount
          i=$((i + 1)); done
      i=0; while [ $i -lt $AUDIO_OPS ]; do
          [ -n "$XC" ] && /pcm_play plughw:$XC,0 1 >/tmp/spdif.last 2>&1 && inc /tmp/spcount
          i=$((i + 1)); done
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
    echo "SOAK ITER $n memfree=${mf}kB irqlines=$il audio=$(cat /tmp/acount) sairx=$(cat /tmp/rxcount) spdif=$(cat /tmp/spcount) micfil=$(cat /tmp/mfcount) i2c=$(cat /tmp/icount) storage=$(cat /tmp/scount) net=$(cat /tmp/ncount)"
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

cat > "$WORK/mipicam.init" <<'EOF'
#!/bin/sh
# MIPI CSI-2 capture: ov5640 -> dw-mipi-csi2 host -> ISI -> /dev/video0.
# ov5640 is a module (unlike the built-in mt9m114), so modprobe it first.
PATH=/sbin:/usr/sbin:/bin:/usr/bin; export PATH
mount -t proc proc /proc 2>/dev/null
mount -t sysfs sysfs /sys 2>/dev/null
mount -t devtmpfs devtmpfs /dev 2>/dev/null
mkdir -p /tmp
sleep 3
modprobe ov5640 2>&1 | sed "s/^/  modprobe ov5640: /"
sleep 2
ls -l /dev/video* /dev/media* 2>/dev/null
echo 0 > /tmp/mcount; echo 0 > /tmp/mfail
( mc=0; mf=0; while true; do
      if /v4l2_cap cap /dev/video0 >/tmp/mipi.last 2>&1; then mc=$((mc + 1)); else mf=$((mf + 1)); fi
      echo "$mc" > /tmp/mcount; echo "$mf" > /tmp/mfail
  done ) &
n=0
while true; do
    n=$((n + 1))
    mf=$(awk '/MemFree/{print $2}' /proc/meminfo 2>/dev/null)
    il=$(grep -c . /proc/interrupts 2>/dev/null)
    echo "SOAK ITER $n memfree=${mf}kB irqlines=$il mipicap=$(cat /tmp/mcount) mipifail=$(cat /tmp/mfail)"
    sleep 5
done
EOF

cat > "$WORK/disp.init" <<'EOF'
#!/bin/sh
# DSI panel scanout: LCDIFv3 CRTC -> dw-mipi-dsi -> rm67199 panel. imx-drm
# creates /dev/fb0 at the panel's native res; fill it repeatedly and let the
# LCDIF page-flip loop DMA the framebuffer out through the DSI link.
PATH=/sbin:/usr/sbin:/bin:/usr/bin; export PATH
mount -t proc proc /proc 2>/dev/null
mount -t sysfs sysfs /sys 2>/dev/null
mount -t devtmpfs devtmpfs /dev 2>/dev/null
mkdir -p /tmp
sleep 5
if [ -e /dev/fb0 ]; then
    echo "FB0 virtual_size=$(cat /sys/class/graphics/fb0/virtual_size 2>/dev/null)"
else
    echo "NO FB0"
fi
echo 0 > /tmp/flipcount; echo 0 > /tmp/flipfail
sz=$(awk '/MemTotal/{print 8294400}' /proc/meminfo)  # 1080*1920*4
( fc=0; ff=0; p=0; while true; do
      # alternate fill patterns so the scanned-out content actually changes
      pat=$(printf '\%o' $(( (p % 2) * 85 + 42 )))   # 0x2a / 0x7f-ish
      if yes "$pat" 2>/dev/null | head -c 8294400 | dd of=/dev/fb0 bs=64k 2>/dev/null; then
          fc=$((fc + 1)); else ff=$((ff + 1)); fi
      echo "$fc" > /tmp/flipcount; echo "$ff" > /tmp/flipfail
      p=$((p + 1)); sleep 1
  done ) &
n=0
while true; do
    n=$((n + 1))
    mf=$(awk '/MemFree/{print $2}' /proc/meminfo 2>/dev/null)
    il=$(grep -c . /proc/interrupts 2>/dev/null)
    echo "SOAK ITER $n memfree=${mf}kB irqlines=$il flips=$(cat /tmp/flipcount) flipfail=$(cat /tmp/flipfail)"
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
            RX=$((RX + $(maxtok sairx "$log")));       save c_sairx "$RX"
            SPD=$((SPD + $(maxtok spdif "$log")));     save c_spdif "$SPD"
            MIC=$((MIC + $(maxtok micfil "$log")));    save c_micfil "$MIC"
            I2C=$((I2C + $(maxtok i2c "$log")));       save c_i2c "$I2C"
            STOR=$((STOR + $(maxtok storage "$log"))); save c_storage "$STOR"
            NET=$((NET + $(maxtok net "$log")));       save c_net "$NET" ;;
        CAMERA)  CAP=$((CAP + $(maxtok captures "$log")));    save c_captures "$CAP" ;;
        MIPICAM) MIPI=$((MIPI + $(maxtok mipicap "$log")));   save c_mipicap "$MIPI" ;;
        DISPLAY) FLIP=$((FLIP + $(maxtok flips "$log")));     save c_flips "$FLIP" ;;
        FLEXIO)  RT=$((RT + $(maxtok roundtrips "$log")));    save c_roundtrips "$RT" ;;
    esac

    if [ "$faulted" -eq 1 ]; then
        INC=$((INC + 1)); save c_incidents "$INC"
        local keep="$WORK/incidents/$(date +%Y%m%d-%H%M%S)-${name}.log"
        cp "$log" "$keep" 2>/dev/null
        say "INCIDENT #$INC in $name: $reason  (serial saved: $keep)"
        write_status incident "$name"
        return 1
    fi
    say "BLOCK $name ok (audio+=$(maxtok audio "$log") sairx+=$(maxtok sairx "$log") spdif+=$(maxtok spdif "$log") micfil+=$(maxtok micfil "$log") i2c+=$(maxtok i2c "$log") stor+=$(maxtok storage "$log") net+=$(maxtok net "$log") cap+=$(maxtok captures "$log") mipi+=$(maxtok mipicap "$log") flips+=$(maxtok flips "$log") rt+=$(maxtok roundtrips "$log"))"
    write_status running "$name"
    return 0
}

# ----- setup ------------------------------------------------------------------
for f in "$QEMU" "$KERNEL" "$DTB_MAIN" "$DTB_CAM" "$DTB_MIPI" "$DTB_DISP" \
         "$DTB_FLEXIO" "$BASE_INITRD" "$PCM_PLAY" "$PCM_CAP" "$V4L2_CAP"; do
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
mk_initrd "$WORK/main.init"    "$WORK/main.cpio.gz"    "$PCM_PLAY" "$PCM_CAP"
mk_initrd "$WORK/cam.init"     "$WORK/cam.cpio.gz"     "$V4L2_CAP"
mk_initrd "$WORK/mipicam.init" "$WORK/mipicam.cpio.gz" "$V4L2_CAP"
mk_initrd "$WORK/disp.init"    "$WORK/disp.cpio.gz"
mk_initrd "$WORK/flexio.init"  "$WORK/flexio.cpio.gz"
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

    d=$(cap_dur "$MIPI_BLOCK")
    [ "$d" -gt 30 ] && { run_block MIPICAM "$DTB_MIPI" "$WORK/mipicam.cpio.gz" "$d" || true; }

    d=$(cap_dur "$DISP_BLOCK")
    [ "$d" -gt 30 ] && { run_block DISPLAY "$DTB_DISP" "$WORK/disp.cpio.gz" "$d" || true; }

    d=$(cap_dur "$FLEXIO_BLOCK")
    [ "$d" -gt 30 ] && { run_block FLEXIO "$DTB_FLEXIO" "$WORK/flexio.cpio.gz" "$d" \
        -device tmp105,bus=flexio1-i2c,address=0x49 || true; }
done

# ----- done -------------------------------------------------------------------
write_status done none
say "================ SOAK36 COMPLETE ================"
say "cycles=$CYCLES boots=$BOOTS incidents=$INC"
say "audio_plays=$AUD sai_rx=$RX spdif=$SPD micfil=$MIC i2c=$I2C storage=$STOR net=$NET"
say "captures=$CAP mipi_captures=$MIPI panel_flips=$FLIP roundtrips=$RT"
say "peak QEMU RSS=${RSSPK}kB"
if [ "$INC" -eq 0 ]; then say "RESULT: PASS (no faults over the full duration)"
else say "RESULT: COMPLETED WITH $INC INCIDENT(S) - see $WORK/incidents/"; fi
rm -f "$WORK/deadline"   # clean slate for any future run
