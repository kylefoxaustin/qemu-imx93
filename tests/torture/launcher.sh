#!/bin/sh
# Torture launcher v2: fan out every concurrent datapath on the i.MX93 model.
# Runs inside the weston guest. Each workload bumps a counter in $P so the host
# watcher can confirm every datapath keeps advancing (frozen counter = wedge).
# CPU stress is niced so the compositor still gets cycles (display stays live
# AND the cores stay saturated).
OV=/mnt/ov
P=$OV/progress
mkdir -p $P
: > $OV/launch.log
log() { echo "$(date +%H:%M:%S) $*" >> $OV/launch.log; }

export XDG_RUNTIME_DIR=/run/user/0
export WAYLAND_DISPLAY=wayland-0
export LD_LIBRARY_PATH=$OV/usr/lib

log "launcher v2 start"
cp -f $OV/firmware/ethosu_firmware /lib/firmware/ 2>/dev/null && log "ethosu_firmware staged"

# audio cards (modprobe the full stack, then find each card index)
modprobe snd-soc-wm8962 snd-soc-fsl-asoc-card snd-soc-fsl-micfil \
         snd-soc-fsl-xcvr snd-soc-imx-card snd-soc-dmic 2>/dev/null
sleep 1
card_of() { awk -v k="$1" 'tolower($0) ~ tolower(k){print $1; exit}' /proc/asound/cards; }
WM=$(card_of wm8962); XC=$(card_of xcvr); MF=$(card_of micfil)
log "audio cards wm8962=$WM xcvr=$XC micfil=$MF"

# 1. DISPLAY - weston-flower animating client (respawned if it dies).
( n=0; while :; do weston-flower >/dev/null 2>&1; n=$((n+1)); echo $n > $P/flower; sleep 1; done ) &
log "flower $!"
# display liveness (guest-side, robust to host load), two signals:
#  disp   = weston-flower client alive (steady while the compositor accepts it)
#  vblank = the lcdif scanout IRQ advancing (frames actually leaving the display)
log "display irqs: $(grep -iE 'lcdif|disp|drm|crtc|4ae3' /proc/interrupts | tr -s ' ' | cut -c1-60 | tr '\n' '|')"
( d=0; v=0; pv=-1; while :; do
    pgrep weston-flower >/dev/null 2>&1 && { d=$((d+1)); echo $d > $P/disp; }
    vb=$(awk '/lcdif|disp|drm|crtc|4ae3/{s+=$2} END{print s+0}' /proc/interrupts)
    [ "$vb" != "$pv" ] && [ "$pv" != "-1" ] && { v=$((v+1)); echo $v > $P/vblank; }
    pv=$vb; sleep 2; done ) &
log "disp watcher $!"

# 2. NPU - mobilenet inference loop via the eIQ delegate (boots M33 + ethos fw).
( n=0; while :; do
    $OV/opt/benchmark_model --graph=$OV/opt/mobilenet_vela.tflite \
        --external_delegate_path=$OV/usr/lib/libethosu_delegate.so \
        --num_runs=10 --warmup_runs=0 >/dev/null 2>&1
    n=$((n+1)); echo $n > $P/npu
  done ) &
log "npu $!"

# 3. CPU stress - niced so weston/NPU preempt it but it still eats idle cycles.
( n=0; while :; do nice -n 19 dd if=/dev/zero of=/dev/null bs=1M count=1500 2>/dev/null; n=$((n+1)); echo $n > $P/cpu1; done ) &
( n=0; while :; do nice -n 19 md5sum /usr/bin/* >/dev/null 2>&1; n=$((n+1)); echo $n > $P/cpu2; done ) &
log "cpu niced"

# 4. Storage - write/sync/verify churn on the SD rootfs (SDHCI + eDMA).
( n=0; while :; do dd if=/dev/urandom of=/root/tort.dat bs=64k count=128 2>/dev/null; sync
    md5sum /root/tort.dat >/dev/null 2>&1; rm -f /root/tort.dat; n=$((n+1)); echo $n > $P/sd; done ) &
log "sd $!"

# 5. Network - real FEC traffic to the slirp gateway (large packets push TX/RX
# buffer descriptors + DMA through imx.enet); loopback only if the FEC is down.
( n=0; while :; do
    if ping -c 50 -i 0.01 -s 1400 10.0.2.2 >/dev/null 2>&1; then echo fec > $OV/net_path
    else ping -c 50 -i 0.01 127.0.0.1 >/dev/null 2>&1; echo loopback > $OV/net_path; fi
    n=$((n+1)); echo $n > $P/net; done ) &
log "net $!"

# 6. AUDIO - all eDMA channels: wm8962/SAI3 play+capture, SPDIF play, MICFIL capture.
if [ -n "$WM" ]; then
  ( n=0; while :; do $OV/opt/pcm_play hw:$WM,0 1 >/dev/null 2>&1; n=$((n+1)); echo $n > $P/aud_play; done ) &
  ( n=0; while :; do $OV/opt/pcm_capture hw:$WM,0 1 >/dev/null 2>&1; n=$((n+1)); echo $n > $P/aud_cap; done ) &
fi
[ -n "$XC" ] && ( n=0; while :; do $OV/opt/pcm_play plughw:$XC,0 1 >/dev/null 2>&1; n=$((n+1)); echo $n > $P/spdif; done ) &
[ -n "$MF" ] && ( n=0; while :; do $OV/opt/pcm_capture plughw:$MF,0 1 >/dev/null 2>&1; n=$((n+1)); echo $n > $P/micfil; done ) &
log "audio launched"

log "all workloads launched"
while :; do echo $(date +%s) > $P/heartbeat; sleep 5; done
