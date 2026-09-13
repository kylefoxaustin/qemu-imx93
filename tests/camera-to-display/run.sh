#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# i.MX93 camera -> LCD: a real photograph in over the CSI, out on the HDMI display.
#
# The vision TRANSPORT path end to end, with no ISP in it:
#
#     smart camera (already-developed YUYV)      the ISI host frame source
#            -> parallel-CSI / MIPI receiver     hw/display/imx93_isi.c
#            -> ISI  -> DRAM capture buffer      DMA, byte-for-byte
#            -> V4L2 DQBUF (tests/.../v4l2_to_fb)
#            -> /dev/fb0 -> LCDIFv3 -> adv7535   HDMI monitor, 1920x1080
#
# "Smart camera" means the sensor emits developed YUV rather than Bayer, so there
# is deliberately NO ISP in this path: it isolates the transport, so a failure
# here cannot hide behind image processing. Flowing raw Bayer through a real
# debayer is the next step, not this one.
#
# ONE prebuilt device tree does the whole job. Unlike the i.MX95 - where the
# camera and panel must be spliced into the base dts by hand - the i.MX93 BSP's
# base EVK dtb already drives the adv7535 HDMI bridge, and the mt9m114 camera
# variant is "base + camera", so imx93-11x11-evk-mt9m114.dtb carries BOTH the
# mt9m114 sensor (-> /dev/video0) and the adv7535 display (-> /dev/fb0). No
# overlay, no fdtoverlay, no panel splice.
#
# Two independent proofs, because either alone is weak:
#   1. BYTES - the guest hashes the captured frame; the host hashes what it fed
#      in at the same stride. Equal hashes mean the image crossed the pipeline
#      intact, not merely that something arrived.
#   2. PIXELS - a screendump of the display is compared against the source image.
#      A correct hash with a black screen would still be a broken scanout path.
#
# NOTE on the ISI's silent gradient fallback: with a frames source configured but
# a per-frame read short (wrong geometry, EOF), hw/display/imx93_isi.c serves its
# synthetic gradient WITHOUT a warning. That is why proof #1 hashes the guest
# capture against the HOST-STAGED frames rather than trusting that "a frame
# arrived" - a gradient substitution hashes to none of the staged frames and
# fails here honestly. The host-side hash is the instrument, not a spare.
#
# Required (override via env): QEMU, KERNEL (Image), DTB (mt9m114 variant),
# BASE_INITRD, a CROSS gcc, python3-Pillow. SKIPs if any is missing.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
CAM="$ROOT/tests/camera-imx93"

DEPLOY=${DEPLOY:-$HOME/Documents/nxp/linux/imx-yocto-bsp/build-imx93/tmp/deploy/images/imx93evk}
QEMU=${QEMU:-$ROOT/build-imx93/qemu-system-aarch64}
KERNEL=${KERNEL:-$DEPLOY/Image}
# mt9m114 variant = base EVK (adv7535 HDMI display) + mt9m114 camera: both nodes.
DTB=${DTB:-$DEPLOY/imx93-11x11-evk-mt9m114.dtb}
BASE_INITRD=${BASE_INITRD:-$HOME/Documents/nxp/imx93-initramfs.cpio.gz}
CROSS=${CROSS:-aarch64-linux-gnu-}
SRC_IMG=${SRC_IMG:-$HERE/scene.png}
TMO=${TMO:-220}
# HOLD=1 keeps the frame on the display indefinitely instead of powering off, for
# a board-farm pane that screendumps on its own timer (a blit that lands and
# exits inside that window is missed, and missed SILENTLY - a black pane looks
# exactly like a dead camera path).
HOLD=${HOLD:-0}

skip() { echo "SKIP: $*"; exit 0; }
[ -x "$QEMU" ]     || skip "no QEMU ($QEMU)"
[ -e "$KERNEL" ]   || skip "no kernel Image ($KERNEL)"
[ -e "$DTB" ]      || skip "no mt9m114 dtb ($DTB)"
[ -e "$BASE_INITRD" ] || skip "no base initramfs ($BASE_INITRD)"
[ -e "$CAM/v4l2_cap.c" ] || skip "no capture oracle ($CAM/v4l2_cap.c)"
[ -e "$SRC_IMG" ]  || skip "no source image ($SRC_IMG)"
command -v "${CROSS}gcc" >/dev/null || skip "no ${CROSS}gcc"
python3 -c "import PIL" 2>/dev/null || skip "no python3 Pillow (for mkframe.py)"
command -v python3 >/dev/null || skip "no python3 (for the QMP screendump)"

WORK=$(mktemp -d); QPID=""
trap 'rm -rf "$WORK"; [ -n "$QPID" ] && kill "$QPID" 2>/dev/null' EXIT
LOG="$WORK/serial.log"
SHOT="${SHOT:-$WORK/panel.ppm}"

# The i.MX93 mt9m114 pipeline negotiates 1280x720 YUYV with a 3840-byte line
# stride (the ISI's pitch/width = 3, so it reads stride*height = 2,764,800 bytes
# per frame). Build the host frame to match exactly; v4l2_to_fb reads back
# whatever G_FMT reports, and a wrong size here makes the ISI's per-frame read
# fall short and silently serve its gradient - which the hash check below then
# catches, so these numbers are self-guarding rather than trusted.
W=${W:-1280}; H=${H:-720}; STRIDE=${STRIDE:-3840}

# STAGE SEVERAL DISTINCT FRAMES, NOT ONE.
#
# Fed a single file the ISI rewinds and re-serves it, so every captured frame is
# byte-identical - and a run that captured ONE frame and re-read it three times
# produces exactly the same log, at r=1.0000. The hashes then prove TRANSPORT and
# say nothing about per-frame capture, which is the property this test is read as
# establishing. The model already cycles a DIRECTORY of *.raw frames, so pointing
# it at one costs nothing and makes the frames tell each other apart.
FRAME=${FRAME:-$WORK/frames}
if [ "$FRAME" = "$WORK/frames" ]; then
    mkdir -p "$WORK/frames"
    for i in 0 1 2; do
        f="$WORK/frames/frame00$i.raw"
        python3 "$HERE/mkframe.py" "$SRC_IMG" "$W" "$H" "$f" "$STRIDE" || exit 1
        python3 - "$f" "$i" "$STRIDE" <<'STAMP' || exit 1
import sys
path, idx, stride = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
b = bytearray(open(path, 'rb').read())
val = (40 + idx * 60) & 0xff
for row in range(8, 24):                  # a small patch, same place each frame
    base = row * stride + 32
    b[base:base + 64] = bytes([val]) * 64
open(path, 'wb').write(bytes(b))
STAMP
    done
else
    echo "using pre-made frame: $FRAME"
fi
# hash every staged frame: the guest's capture must match ONE of them
HOST_HASHES=$(python3 - "$FRAME" <<'PY'
import sys, os
# FNV-1a 64-bit offset basis; must match v4l2_to_fb.c exactly
def fnv(path):
    h = 0xcbf29ce484222325
    for b in open(path, 'rb').read():
        h = ((h ^ b) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return "%016x" % h
p = sys.argv[1]
if os.path.isdir(p):
    for n in sorted(os.listdir(p)):
        print(fnv(os.path.join(p, n)))
else:
    print(fnv(p))
PY
)
echo "host frame hashes:"; echo "$HOST_HASHES" | sed 's/^/  /'

# Build the fb client and the proven capture oracle. tests/camera-imx93/v4l2_cap.c
# enables the (default-disabled) sensor link and propagates the sensor format onto
# every crossbar sink so media-core link validation passes at STREAMON - the exact
# setup v4l2_to_fb needs, already proven, so run it first rather than duplicating.
"${CROSS}gcc" -O2 -Wall -static -o "$WORK/v4l2_to_fb" "$HERE/v4l2_to_fb.c" \
    || { echo "FAIL: could not build v4l2_to_fb.c"; exit 1; }
"${CROSS}gcc" -O2 -Wall -static -o "$WORK/v4l2_cap" "$CAM/v4l2_cap.c" \
    || { echo "FAIL: could not build v4l2_cap.c"; exit 1; }

# Overlay /myinit + the two static binaries on top of the base rootfs.
mkdir -p "$WORK/ov"
cp "$WORK/v4l2_to_fb" "$WORK/ov/v4l2_to_fb"
cp "$WORK/v4l2_cap"   "$WORK/ov/v4l2_cap"
[ "$HOLD" = 1 ] && touch "$WORK/ov/HOLD"
cat > "$WORK/ov/myinit" <<'INIT'
#!/bin/sh
PATH=/sbin:/usr/sbin:/bin:/usr/bin; export PATH
mount -t proc proc /proc 2>/dev/null
mount -t sysfs sysfs /sys 2>/dev/null
mount -t devtmpfs devtmpfs /dev 2>/dev/null
echo "=== CAM2LCD ==="
sleep 5
ls -l /dev/fb0 /dev/video0 2>&1
# set the pipeline up (links + format propagation), then blit a frame
/v4l2_cap cap /dev/video0
/v4l2_to_fb /dev/video0 /dev/fb0
echo "=== CAM2LCD-DONE ==="
if [ -f /HOLD ]; then
    echo "holding the frame on the display"
    while true; do sleep 3600; done
fi
sleep 5          # leave the image up long enough for the screendump
poweroff -f 2>/dev/null || while true; do sleep 5; done
INIT
chmod +x "$WORK/ov/myinit"
( cd "$WORK/ov" && find . | cpio -o -H newc 2>/dev/null > "$WORK/o.cpio" )
cat "$BASE_INITRD" "$WORK/o.cpio" > "$WORK/initrd.gz"

# Screendump the display once the guest reports the blit is up, via QMP.
( for i in $(seq 1 "$TMO"); do
      grep -qa "CAM2LCD-DONE" "$LOG" 2>/dev/null && break
      sleep 1
  done
  sleep 4    # let the LCDIF page-flip loop stabilise before capturing
  python3 - "$WORK/qmp.sock" "$SHOT" <<'PY' >/dev/null 2>&1
import socket, json, sys, time
sock, out = sys.argv[1], sys.argv[2]
s = socket.socket(socket.AF_UNIX); s.connect(sock)
f = s.makefile("rw"); f.readline()
def cmd(c): f.write(json.dumps(c) + "\n"); f.flush(); return f.readline()
cmd({"execute": "qmp_capabilities"})
cmd({"execute": "screendump", "arguments": {"filename": out}})
time.sleep(1)
PY
) &
SHOOTER=$!

timeout -k 5 "$TMO" "$QEMU" -M imx93-11x11-evk -audio driver=none -m 4G -display none \
  -kernel "$KERNEL" -dtb "$DTB" -initrd "$WORK/initrd.gz" \
  -append "console=ttyLP0,115200 cpuidle.off=1 rdinit=/myinit ignore_loglevel" \
  -global driver=imx93.isi,property=frames,value="$FRAME" \
  -qmp "unix:$WORK/qmp.sock,server=on,wait=off" \
  -serial "file:$LOG" -serial null >/dev/null 2>"$WORK/qemu.err" || true
wait "$SHOOTER" 2>/dev/null
QPID=""

# QEMU's own warnings matter: a bad frames path makes the ISI error out at
# startup rather than run, so surface stderr instead of swallowing it.
if [ -s "$WORK/qemu.err" ]; then
    echo "--- qemu stderr ---"; cat "$WORK/qemu.err"
fi

echo "--- camera-to-display report ---"
sed -n '/=== CAM2LCD ===/,/=== CAM2LCD-DONE ===/p' "$LOG" \
    | grep -aE 'CAPTURE|FRAME |FB |BLIT|DISPLAYED|CAPTURE_HASH|video0|fb0|G_FMT|STREAMON|DQBUF'

fail() { echo "FAIL: $*"; exit 1; }
grep -qa "DISPLAYED" "$LOG" || fail "client never reached the framebuffer blit"

# the serial log carries CR line endings; strip them or the compare always fails
GUEST_HASH=$(grep -a 'CAPTURE_HASH' "$LOG" | tail -1 | awk '{print $2}' | tr -d '\r')
[ -n "$GUEST_HASH" ] || fail "no capture hash from the guest"
echo "guest capture hash: $GUEST_HASH"
echo "$HOST_HASHES" | grep -qx "$GUEST_HASH" \
    || fail "capture hash $GUEST_HASH matches NO staged frame (image altered in transit, or ISI served its gradient)"

# THE CAPTURE MUST VARY. Distinct frames were staged; if every captured frame
# hashes the same, the pipeline is re-serving one frame and any r=1.0000 is a
# statement about transport only.
ndist=$(grep -a '^FRAME ' "$LOG" | grep -oa 'hash=[0-9a-f]*' | sort -u | wc -l)
[ "$ndist" -ge 2 ] \
    || fail "all captured frames hash identically ($ndist distinct) - the capture is not per-frame"
echo "captured $ndist distinct frame hashes (staged 3)"

if [ -s "$SHOT" ]; then
    python3 "$HERE/checkshot.py" "$SHOT" "$SRC_IMG" "$W" "$H" || exit 1
else
    echo "WARN: no screendump captured (display proof skipped)"
fi

echo "PASS: camera -> ISI -> DRAM -> /dev/fb0 -> LCDIFv3 -> adv7535 HDMI, image intact end to end"
