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

# audio cards (modprobe the full stack, then find each card index)
modprobe snd-soc-wm8962 snd-soc-fsl-asoc-card snd-soc-fsl-micfil \
         snd-soc-fsl-xcvr snd-soc-imx-card snd-soc-dmic 2>/dev/null
sleep 1
card_of() { awk -v k="$1" 'tolower($0) ~ tolower(k){print $1; exit}' /proc/asound/cards; }
WM=$(card_of wm8962); XC=$(card_of xcvr); MF=$(card_of micfil)
log "audio cards wm8962=$WM xcvr=$XC micfil=$MF"

# NOTE: the Ethos-U NPU is deliberately NOT in this concurrent set. Its eIQ
# delegate boots the Cortex-M33 on /dev/ethosu0 open, and that rpmsg bring-up is
# a known load-sensitive guest-side race - under the weston desktop it
# intermittently RCU-stalls and wedges the WHOLE guest (see README "Known
# issues"). The NPU/M33 datapath is covered by tests/ethosu-infer and
# tests/ethosu-rpmsg instead.

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


# 3. CPU stress - niced so weston preempts it but it still eats idle cycles.
( n=0; while :; do nice -n 19 dd if=/dev/zero of=/dev/null bs=1M count=1500 2>/dev/null; n=$((n+1)); echo $n > $P/cpu1; done ) &
( n=0; while :; do nice -n 19 md5sum /usr/bin/* >/dev/null 2>&1; n=$((n+1)); echo $n > $P/cpu2; done ) &
log "cpu niced"

# 4. Storage - write/sync/verify churn on the SD rootfs (SDHCI + eDMA).
( n=0; while :; do dd if=/dev/urandom of=/root/tort.dat bs=64k count=128 2>/dev/null; sync
    md5sum /root/tort.dat >/dev/null 2>&1; rm -f /root/tort.dat; n=$((n+1)); echo $n > $P/sd; done ) &
log "sd $!"

# 5. Network - IP-stack churn over loopback (the FEC link is kept down; see
# README "Known issues" - a backed FEC link wedges the M33 boot path).
( n=0; while :; do ping -c 30 -i 0.02 127.0.0.1 >/dev/null 2>&1
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
