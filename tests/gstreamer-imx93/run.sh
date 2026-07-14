#!/usr/bin/env bash
#
# GStreamer video pipeline rendered on the emulated i.MX93 display.
#
# The i.MX93 has NO hardware JPEG/video codec (unlike the i.MX95's CAST block) -
# its Reference Manual has zero codec blocks - so multimedia on this SoC is pure
# software on the A55s. This test proves that path end to end: a stock GStreamer
# pipeline decodes/generates frames in software and hands them to waylandsink,
# the system Weston (software-composited, since there is no G2D either) puts them
# on the modelled LCDIFv3 -> DSI -> ADV7535 -> HDMI scanout, and pixels appear in
# the QEMU display.
#
#   PIPELINE=m1  videotestsrc ! videoconvert ! waylandsink           (colour bars)
#   PIPELINE=m2  filesrc clip.ogv ! oggdemux ! theoradec ! ... ! waylandsink
#
# imx-image-core already carries gst-launch, the GStreamer core, glib/orc and a
# Weston that auto-starts; mkrootfs.sh stages the extra plugin .so's (built by
# the BSP as .deb's but installed in no image) into a throwaway ext4 rootfs.
#
# Override via env: KERNEL=, DTB=, ROOTFS=, FEED=, QEMU=, PIPELINE=, MEDIA=,
# DISPLAY_BACKEND=, VERIFY=1 (headless: screendump + non-black check), OUT=.
set -u

HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
DEPLOY=${DEPLOY:-$HOME/Documents/nxp/linux/imx-yocto-bsp/build-imx93/tmp/deploy/images/imx93evk}

QEMU=${QEMU:-$REPO/build-imx93/qemu-system-aarch64}
KERNEL=${KERNEL:-$DEPLOY/Image}
DTB=${DTB:-$DEPLOY/imx93-11x11-evk.dtb}
ROOTFS=${ROOTFS:-$DEPLOY/imx-image-core-imx93evk.rootfs.tar.zst}
FEED=${FEED:-$DEPLOY/../../deb}
PIPELINE=${PIPELINE:-m1}
MEDIA=${MEDIA:-}
DISPLAY_BACKEND=${DISPLAY_BACKEND:-gtk}
VERIFY=${VERIFY:-0}
OUT=${OUT:-$HERE/build}

skip() { echo "SKIP: $*" >&2; exit 0; }

[ -x "$QEMU" ] || { echo "error: build qemu-system-aarch64 first ($QEMU)" >&2; exit 1; }
for t in fakeroot zstd dpkg-deb readelf mkfs.ext4; do
    command -v "$t" >/dev/null || skip "host tool '$t' missing (apt install fakeroot zstd dpkg e2fsprogs binutils)"
done
[ -e "$KERNEL" ] || skip "kernel Image not found: $KERNEL (build the BSP)"
[ -e "$DTB" ]    || skip "device tree not found: $DTB"
[ -e "$ROOTFS" ] || skip "imx-image-core rootfs not found: $ROOTFS (bitbake imx-image-core)"
[ -d "$FEED" ]   || skip "BSP .deb feed not found: $FEED"
ROOTFS=$(readlink -f "$ROOTFS")

# m2 needs a Theora clip; generate a short one with host ffmpeg if not supplied.
if [ "$PIPELINE" = m2 ] && [ -z "$MEDIA" ]; then
    if command -v ffmpeg >/dev/null; then
        MEDIA="$OUT/clip.ogv"; mkdir -p "$OUT"
        echo "==> generating test clip with ffmpeg: $MEDIA"
        ffmpeg -y -loglevel error -f lavfi -i testsrc=size=640x480:rate=15:duration=6 \
            -c:v libtheora -q:v 6 "$MEDIA" || skip "ffmpeg could not produce a Theora clip"
    else
        skip "PIPELINE=m2 needs MEDIA=<clip.ogv> or host ffmpeg to generate one"
    fi
fi

IMG="$OUT/gst-imx93.ext4"
mkdir -p "$OUT"
echo "==> building demo rootfs (fakeroot) -> $IMG"
STAGE="$OUT/rootfs" IMG="$IMG" ROOTFS="$ROOTFS" FEED="$FEED" PIPELINE="$PIPELINE" MEDIA="$MEDIA" \
    fakeroot bash "$HERE/mkrootfs.sh" || { echo "error: rootfs build failed" >&2; exit 1; }

APPEND="console=ttyLP0,115200 root=/dev/mmcblk0 rootwait rw cpuidle.off=1"
QEMU_ARGS=( -M imx93-11x11-evk -audio driver=none -m 4G
    -kernel "$KERNEL" -dtb "$DTB"
    -drive if=sd,file="$IMG",format=raw
    -append "$APPEND" )

if [ "$VERIFY" = 1 ]; then
    # Headless: boot, periodically screendump the display, pass when non-black.
    QMP="$OUT/qmp.sock"; SHOT="$OUT/screen.ppm"; LOG="$OUT/serial.log"
    rm -f "$QMP" "$SHOT"
    echo "==> headless verify: booting, will screendump until non-black (or timeout)"
    "$QEMU" "${QEMU_ARGS[@]}" -display none -serial "file:$LOG" -serial null \
        -qmp "unix:$QMP,server=on,wait=off" &
    qpid=$!
    trap 'kill $qpid 2>/dev/null' EXIT
    # Minimal QMP screendump over the unix socket (no socat dependency).
    qmp_screendump() {
        python3 - "$QMP" "$SHOT" <<'PY' 2>/dev/null
import socket, sys, json
sock, shot = sys.argv[1], sys.argv[2]
s = socket.socket(socket.AF_UNIX); s.settimeout(5); s.connect(sock)
f = s.makefile('rw')
f.readline()                                   # QMP greeting
def cmd(d):
    f.write(json.dumps(d) + '\n'); f.flush()
    while True:
        line = f.readline()
        if not line: return None
        m = json.loads(line)
        if 'return' in m or 'error' in m: return m
cmd({"execute": "qmp_capabilities"})
cmd({"execute": "screendump", "arguments": {"filename": shot}})
PY
    }
    nonblack=0
    for s in $(seq 1 60); do          # up to ~5 min wall (TCG boot+weston+decode)
        sleep 5
        [ -S "$QMP" ] || continue
        qmp_screendump
        [ -s "$SHOT" ] || continue
        # PPM (P6) - skip 3 header lines, count bytes that are not 0x00.
        nz=$(tail -c +16 "$SHOT" 2>/dev/null | tr -d '\000' | wc -c)
        echo "    t=$((s*5))s: non-zero pixel bytes = $nz"
        if [ "$nz" -gt 100000 ]; then nonblack=1; break; fi
    done
    kill $qpid 2>/dev/null; trap - EXIT
    if [ "$nonblack" = 1 ]; then
        echo "PASS: GStreamer pipeline rendered to the emulated display ($SHOT)"
        exit 0
    fi
    echo "FAIL: display stayed black; see $LOG and $SHOT" >&2
    exit 1
fi

echo "==> booting (display=$DISPLAY_BACKEND); watch for the pipeline on the emulated HDMI"
set -x
exec "$QEMU" "${QEMU_ARGS[@]}" -display "$DISPLAY_BACKEND" -serial mon:stdio -serial null
