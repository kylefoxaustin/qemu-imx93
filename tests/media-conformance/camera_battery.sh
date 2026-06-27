#!/bin/sh
# Camera conformance battery (runs in the guest, mt9m114 DTB).
#
# The ISI capture pipe is default-disabled (sensor link off, no format), so the
# guest rootfs has no media-ctl/v4l2-ctl: we reuse the existing v4l2_cap oracle
# to enable the sensor link + propagate the format (and do a 5-frame capture
# smoke), then run the upstream v4l2-compliance suite against the now-live
# /dev/video0. Each v4l2-compliance sub-test becomes one MEDIA: marker.
PATH=/:/sbin:/usr/sbin:/bin:/usr/bin; export PATH
pass() { echo "MEDIA:PASS:$1:$2"; }
fail() { echo "MEDIA:FAIL:$1:$2"; }
skip() { echo "MEDIA:SKIP:$1:$2"; }

echo "=== CAMERA CONFORMANCE BATTERY ==="
# Wait for the capture node, then bring the pipeline up. The mt9m114 sensor + ISI
# media graph finish probing after userspace starts; running v4l2_cap too early
# enables the link before the sensor is ready -> STREAMON -EPIPE. Retry until the
# setup+capture sticks (link/format state then persists for the suite).
for i in $(seq 1 20); do [ -e /dev/video0 ] && break; sleep 1; done
ls -l /dev/video* /dev/media* /dev/v4l-subdev* 2>&1

# 'cap' subcommand runs setup_pipeline first (enables the sensor link + props the
# format down the chain) - without it STREAMON fails link_validate (-EPIPE).
/v4l2_cap topo /dev/media0 2>&1 | head -20
cap=""
for try in 1 2 3 4 5 6 7 8; do
    cap=$(/v4l2_cap cap /dev/video0 2>&1)
    echo "$cap" | grep -q "CAMERA-CAP.*PASS" && break
    sleep 2
done
echo "$cap" | grep -iE 'CAMERA-CAP|SETUP|found|video0' | tail -12
if echo "$cap" | grep -q "CAMERA-CAP.*PASS"; then
    pass cam-capture-smoke "$(echo "$cap" | grep -oE '[0-9]+/[0-9]+ frames' | tail -1)"
else
    fail cam-capture-smoke "v4l2_cap did not PASS: $(echo "$cap" | grep -iE 'FAIL|error' | tail -1)"
fi

# 2. v4l2-compliance on the live video node. Map each sub-test to a marker:
#    OK -> PASS, "OK (Not Supported)"/"Not Supported" -> SKIP, FAIL -> FAIL.
DEV=/dev/video0
echo "--- v4l2-compliance $DEV ---"
comp=$(v4l2-compliance -d $DEV 2>&1)
echo "$comp" | grep -iE 'Total|Succeeded|Failed|driver|Compliance' | tail -8
echo "$comp" | awk '
  /test .*: / {
    res=$NF
    name=$0; sub(/^[ \t]*test /,"",name); sub(/:.*/,"",name)
    gsub(/[^A-Za-z0-9]+/,"_",name); sub(/_+$/,"",name)
    key="cam-" name
    if ($0 ~ /Not Supported|Not Applicable/) print "MEDIA:SKIP:" key ":not supported"
    else if (res=="OK") print "MEDIA:PASS:" key ":ok"
    else if (res=="FAIL") print "MEDIA:FAIL:" key ":compliance fail"
  }'

# 3. Streaming conformance: actually STREAMON/DQBUF/STREAMOFF through the suite
#    (the pipe is linked from v4l2_cap). This exercises the ISI's real frame DMA.
echo "--- v4l2-compliance -s $DEV (streaming) ---"
scomp=$(v4l2-compliance -d $DEV -s 2>&1)
echo "$scomp" | grep -iE 'Streaming|Total|Succeeded|Failed' | tail -6
sfail=$(echo "$scomp" | grep -iE 'Total.*Succeeded' | tail -1 | grep -oE 'Failed: [0-9]+' | grep -oE '[0-9]+')
if [ -n "$sfail" ] && [ "$sfail" -eq 0 ]; then
    pass cam-streaming-conformance "$(echo "$scomp" | grep -iE 'Total.*Succeeded' | tail -1)"
else
    fail cam-streaming-conformance "$(echo "$scomp" | grep -iE 'Total.*Succeeded' | tail -1) (failed=$sfail)"
fi

# Overall summary marker from the suite tally.
tot=$(echo "$comp" | grep -iE 'Total.*Succeeded' | tail -1)
nf=$(echo "$tot" | grep -oE 'Failed: [0-9]+' | grep -oE '[0-9]+')
if [ -n "$nf" ] && [ "$nf" -eq 0 ]; then
    pass cam-compliance-overall "$tot"
else
    fail cam-compliance-overall "${tot:-no summary} (failed=$nf)"
fi

echo "=== CAMERA CONFORMANCE BATTERY DONE ==="
echo "MEDIA:BATTERY:DONE"
