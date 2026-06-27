#!/bin/sh
# Display conformance battery (runs in the guest, rm67199 DTB).
#
# Drives the LCDIFv3 -> DSI pipeline through DRM/KMS with modetest and emits one
# MEDIA: marker per case to the console; the host scorer tallies them. Unlike
# the fbdev smoke test, this exercises the KMS mode-set + every plane pixel
# format the LCDIF advertises, plus a vblank-paced page-flip loop.
#
# Marker grammar (host greps "^MEDIA:"):
#   MEDIA:PASS:<name>:<detail>
#   MEDIA:FAIL:<name>:<detail>
#   MEDIA:SKIP:<name>:<detail>
#   MEDIA:SHOOT:<name>     -> host: take a QMP screendump now + score non-black
#   MEDIA:BATTERY:DONE
PATH=/:/sbin:/usr/sbin:/bin:/usr/bin; export PATH
MT="/modetest -M imx-drm"
pass() { echo "MEDIA:PASS:$1:$2"; }
fail() { echo "MEDIA:FAIL:$1:$2"; }
skip() { echo "MEDIA:SKIP:$1:$2"; }

echo "=== DISPLAY CONFORMANCE BATTERY ==="
# The DSI bridge/panel probe chains through EPROBE_DEFER (-517) and the connector
# only goes "connected" once imx-drm finishes binding (~17s). Poll, don't guess.
for i in $(seq 1 40); do
    [ -e /dev/dri/card0 ] && $MT 2>/dev/null | grep -q connected && break
    sleep 1
done
ls -l /dev/dri/ 2>&1
echo "--- DIAG: connector sysfs ---"; for s in /sys/class/drm/card0-*/status; do echo "$s=$(cat $s 2>/dev/null)"; done
echo "--- DIAG: modetest raw ---"; $MT 2>&1 | sed -n '/Connectors:/,/CRTCs:/p' | head -8
echo "--- DIAG: dmesg panel/dsi ---"; dmesg 2>/dev/null | grep -iE 'raydium|dsi|panel|drm.*bridge|reset gpio' | tail -8

# Resource enumeration + dynamic ids.
RES=$($MT 2>/dev/null)
echo "$RES" | grep -q "connected" \
    && pass drm-enumerate "$(echo "$RES" | grep -c connected) connected connector(s)" \
    || fail drm-enumerate "no connected connector"

CONN=$(echo "$RES" | awk '/^Connectors:/{f=1;next} f&&/connected/{print $1; exit}')
CRTC=$(echo "$RES" | awk '/^CRTCs:/{f=1;next} f&&/^[0-9]/{print $1; exit}')
MODE=$(echo "$RES" | awk '/connected/{f=1} f&&/^  #0/{print $2; exit}')
FORMATS=$(echo "$RES" | awk '/^Planes:/{f=1} f&&/formats:/{sub(/.*formats: /,""); print; exit}')
echo "drm: conn=$CONN crtc=$CRTC mode=$MODE formats=[$FORMATS]"
[ -n "$CONN" ] && [ -n "$CRTC" ] && [ -n "$MODE" ] \
    && pass drm-topology "conn=$CONN crtc=$CRTC mode=$MODE" \
    || { fail drm-topology "could not parse conn/crtc/mode"; echo "MEDIA:BATTERY:DONE"; exit 0; }

# Per-format mode-set: set the mode with each advertised plane format and tear
# down (stdin closed -> modetest draws its pattern then exits). rc!=0 = the
# LCDIF/DRM rejected or mishandled that pixel format.
for fmt in $FORMATS; do
    out=$(echo | $MT -s "$CONN@$CRTC:$MODE@$fmt" 2>&1)
    rc=$?
    if [ $rc -eq 0 ] && ! echo "$out" | grep -qiE 'fail|error|unable|invalid'; then
        pass "drm-format-$fmt" "modeset+scanout ok"
    else
        fail "drm-format-$fmt" "rc=$rc $(echo "$out" | grep -iE 'fail|error|unable|invalid' | head -1)"
    fi
done

# Page-flip / vblank FIRST, on a clean CRTC: -v runs a vsync-paced flip loop and
# prints the achieved rate. This exercises the LCDIF page-flip completion event
# (DRM_EVENT_FLIP_COMPLETE) off the scanout vblank IRQ.
flog=$( (timeout -s INT 8 $MT -v -s "$CONN@$CRTC:$MODE" 2>&1) )
fps=$(echo "$flog" | grep -oiE 'freq [0-9.]+|[0-9.]+ fps|[0-9.]+Hz' | head -1)
if echo "$flog" | grep -qiE 'fps|freq|[0-9]+\.[0-9]+Hz'; then
    pass drm-pageflip "vblank flip loop ran ${fps:-(rate seen)}"
else
    fail drm-pageflip "no page-flip/vblank progress: $(echo "$flog" | tail -1)"
fi
sleep 1

# Visual scanout proof LAST (it leaves the CRTC dirty): hold the native mode
# (drawing modetest's SMPTE pattern) while the host screendumps. modetest waits
# on stdin after setup, so feed it a slow pipe (no EOF) to keep the mode held -
# a plain background job gets stdin EOF and tears the CRTC down before the
# screendump lands (-> black). Restore with SIGINT so the CRTC is left clean.
( sleep 10 | $MT -s "$CONN@$CRTC:$MODE" >/dev/null 2>&1 ) &
HOLD=$!
sleep 3
echo "MEDIA:SHOOT:drm-scanout"
sleep 4
pkill -INT modetest 2>/dev/null; sleep 1; kill $HOLD 2>/dev/null

echo "=== DISPLAY CONFORMANCE BATTERY DONE ==="
echo "MEDIA:BATTERY:DONE"
