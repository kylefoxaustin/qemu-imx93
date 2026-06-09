#!/usr/bin/env bash
#
# MIPI-DSI panel scanout check for the i.MX93 QEMU machine.
#
# Boots the imx93-11x11-evk-rm67199 device tree (a Raydium RM67199 MIPI-DSI
# panel). The pipeline is LCDIFv3 CRTC -> dw-mipi-dsi host -> rm67199 panel; the
# panel driver brings the link up over the DSI host (its DCS init commands drain
# through the host's command FIFOs), imx-drm creates /dev/fb0 at the panel's
# native 1080x1920, and the LCDIF DMAs that framebuffer and scans it out.
#
# This fills /dev/fb0 with a known pattern, then QMP-screendumps the emulated
# display and verifies it is the panel's native 1080x1920 and non-black - i.e.
# the LCDIF actually scanned the framebuffer out through the DSI panel. Headless
# (-display none); needs python3 on the host for the QMP screendump.
#
# Note: screendump only after the LCDIF page-flip loop has stabilised EN (a few
# seconds past fb setup) - a capture during the brief modeset settle reads the
# default surface. Set LCDIF_DBG=1 to trace the scanout enable/dimensions.
#
# Override paths via env: KERNEL=/path/Image DTB=/path.dtb BASE_INITRD=...
set -u

HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
DEPLOY=${DEPLOY:-$HOME/Documents/nxp/linux/imx-yocto-bsp/build-imx93/tmp/deploy/images/imx93evk}
QEMU=${QEMU:-$REPO/build-imx93/qemu-system-aarch64}
KERNEL=${KERNEL:-$DEPLOY/Image}
DTB=${DTB:-$DEPLOY/imx93-11x11-evk-rm67199.dtb}
BASE_INITRD=${BASE_INITRD:-$HOME/Documents/nxp/imx93-initramfs.cpio.gz}

need() { [ -e "$2" ] || { echo "error: $3 not found: $2" >&2; exit 1; }; }
need QEMU "$QEMU" "qemu"; need KERNEL "$KERNEL" "kernel"; need DTB "$DTB" "dtb"
need BASE_INITRD "$BASE_INITRD" "initrd"
command -v python3 >/dev/null || { echo "error: python3 needed for QMP" >&2; exit 1; }

TMP=$(mktemp -d); QPID=""
trap 'rm -rf "$TMP"; [ -n "$QPID" ] && kill "$QPID" 2>/dev/null' EXIT

mkdir -p "$TMP/ov"
cat > "$TMP/ov/myinit" <<'EOF'
#!/bin/sh
PATH=/sbin:/usr/sbin:/bin:/usr/bin; export PATH
mount -t proc proc /proc 2>/dev/null
mount -t sysfs sysfs /sys 2>/dev/null
mount -t devtmpfs devtmpfs /dev 2>/dev/null
sleep 5
if [ -e /dev/fb0 ]; then
    cat /sys/class/graphics/fb0/virtual_size
    yes $(printf '\252') 2>/dev/null | head -c 8294400 | dd of=/dev/fb0 bs=64k 2>/dev/null
    echo "FB WRITTEN"
else
    echo "NO FB0"
fi
while true; do sleep 5; done
EOF
chmod +x "$TMP/ov/myinit"
( cd "$TMP/ov" && find . | cpio -o -H newc 2>/dev/null > "$TMP/o.cpio" )
cat "$BASE_INITRD" "$TMP/o.cpio" > "$TMP/c.cpio.gz"

echo "display: booting rm67199 DSI panel -> $TMP/shot.ppm"
"$QEMU" -M imx93-11x11-evk -m 4G -display none \
    -kernel "$KERNEL" -dtb "$DTB" -initrd "$TMP/c.cpio.gz" \
    -append "console=ttyLP0,115200 cpuidle.off=1 rdinit=/myinit ignore_loglevel" \
    -serial "file:$TMP/serial.log" -serial null \
    -qmp "unix:$TMP/qmp.sock,server=on,wait=off" &
QPID=$!

for i in $(seq 1 70); do
    grep -aq "FB WRITTEN\|NO FB0" "$TMP/serial.log" 2>/dev/null && break
    sleep 1
done
sleep 4   # let the LCDIF page-flip loop stabilise EN before capturing

python3 - "$TMP/qmp.sock" "$TMP/shot.ppm" <<'PY'
import socket, json, sys, time
sock, out = sys.argv[1], sys.argv[2]
s = socket.socket(socket.AF_UNIX); s.connect(sock)
f = s.makefile("rw"); f.readline()
def cmd(c): f.write(json.dumps(c) + "\n"); f.flush(); return f.readline()
cmd({"execute": "qmp_capabilities"})
cmd({"execute": "screendump", "arguments": {"filename": out}})
time.sleep(1)
PY

kill "$QPID" 2>/dev/null; wait "$QPID" 2>/dev/null; QPID=""

python3 - "$TMP/shot.ppm" <<'PY'
import sys
d = open(sys.argv[1], "rb").read()
def tok(d, i):
    while d[i] in b' \n\t': i += 1
    j = i
    while d[j] not in b' \n\t': j += 1
    return d[i:j], j
i = 0
magic, i = tok(d, i); w, i = tok(d, i); h, i = tok(d, i); mx, i = tok(d, i); i += 1
px = d[i:]
n = min(900000, len(px))
mean = sum(px[:n]) / n if n else 0
ok = magic == b'P6' and w == b'1080' and h == b'1920' and mean > 40
print("DSI-PANEL[rm67199]: %s %sx%s mean=%.1f -> %s" %
      (magic.decode(), w.decode(), h.decode(), mean,
       "PASS (LCDIF scanned the DSI panel framebuffer out)" if ok else "FAIL"))
sys.exit(0 if ok else 1)
PY
