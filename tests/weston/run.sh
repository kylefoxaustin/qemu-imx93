#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Weston (Wayland) desktop on the i.MX93, shown on the emulated LCDIFv3 -> DSI
# panel output. Boots the NXP core-image-weston .wic rootfs from the (writable)
# SD card, lets the image's own systemd + udev + logind bring up
# weston.service, and QMP-screendumps the desktop.
#
# Two things this exercises end to end:
#   - the uSDHC SD write+read datapath (a writable ext4 rootfs from a real
#     multi-hundred-MB .wic), and
#   - the LCDIFv3 scanout of a real Wayland compositor through the DSI panel.
#
# Like the i.MX95, the i.MX93 has no 3D GPU, so weston must software-render:
# this test forces the weston.ini renderer to pixman (use-g2d=false). The
# default use-g2d=true wants a Mali GPU and weston exits with "No mali devices
# found". The .wic's ext4 uses newer features than the host e2fsprogs can write,
# so the renderer switch is done IN-GUEST: a tiny busybox initramfs mounts the
# rootfs, seds weston.ini, then switch_root's into systemd.
#
# Unlike the i.MX95 there is NO System Manager firmware and NO dts splice: the
# i.MX93 BSP ships panel-attached device trees, so we just boot the prebuilt
# rm67199 (MIPI-DSI) panel dtb that tests/lcd-panel/attach-lcd.sh resolves.
#
# Required (override via env): QEMU, KERNEL (Image), a panel DTB (auto via
# attach-lcd.sh), the busybox INITRD, and WIC - a core-image-weston .wic disk
# image (or .wic.zst). SKIPs if the WIC is absent.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)

DEPLOY=${DEPLOY:-$HOME/Documents/nxp/linux/imx-yocto-bsp/build-imx93/tmp/deploy/images/imx93evk}
QEMU=${QEMU:-$ROOT/build-imx93/qemu-system-aarch64}
KERNEL=${KERNEL:-$DEPLOY/Image}
PANEL=${PANEL:-rm67199}
INITRD=${INITRD:-$ROOT/tests/busybox-initramfs/busybox-initramfs.cpio.gz}
# A core-image-weston .wic (raw or .zst). Default to the BSP deploy if present.
WIC=${WIC:-$DEPLOY/core-image-weston-imx93evk.rootfs.wic.zst}
TMO=${TMO:-1200}

need() { [ -e "$2" ] || { echo "SKIP: missing $1: $2"; exit 0; }; }
[ -e "$WIC" ] || { echo "SKIP: set WIC=<core-image-weston .wic[.zst]> to run the Weston desktop test"; exit 0; }
need QEMU "$QEMU"; need KERNEL "$KERNEL"; need INITRD "$INITRD"
[ -x "$HERE/../lcd-panel/attach-lcd.sh" ] || { echo "SKIP: tests/lcd-panel/attach-lcd.sh missing"; exit 0; }
command -v zstd >/dev/null || { echo "SKIP: zstd needed to expand the .wic"; exit 0; }

WORK=$(mktemp -d); trap 'rm -rf "$WORK"; [ -n "${QPID:-}" ] && kill "$QPID" 2>/dev/null || true' EXIT
LOG="$WORK/serial.log"; QMP="$WORK/qmp.sock"; OUT=${OUT:-$WORK/weston.png}; QPID=""

# --- Panel dtb: the 93 ships panel-attached dtbs; attach-lcd.sh resolves the
#     prebuilt one (no dts splice, no dtc). ---------------------------------
DEPLOY="$DEPLOY" "$HERE/../lcd-panel/attach-lcd.sh" "$WORK/panel.dtb" "$PANEL"

# --- Prepare the disk: expand .zst, then grow to the next power-of-2 (the SD
#     card model requires a power-of-2 capacity). -----------------------------
DISK="$WORK/weston.wic"
case "$WIC" in
    *.zst) zstd -d -f "$WIC" -o "$DISK" ;;
    *)     cp --reflink=auto "$WIC" "$DISK" ;;
esac
cur=$(stat -c%s "$DISK"); p2=1
while [ "$p2" -lt "$cur" ]; do p2=$((p2 * 2)); done
truncate -s "$p2" "$DISK"

# --- switch_root initramfs: patch weston.ini to pixman, then hand off to the
#     image's systemd. The guest kernel mounts the .wic ext4 (its features are
#     newer than the host's e2fsprogs). The rootfs partition device depends on
#     which uSDHC enumerates first, so probe mmcblk0p2 / 1p2 / 2p2. -----------
STAGE="$WORK/root"; mkdir -p "$STAGE"
zcat "$INITRD" | (cd "$STAGE" && cpio -idmu 2>/dev/null)
cat > "$STAGE/init" <<'INIT'
#!/bin/busybox sh
/bin/busybox mount -t proc proc /proc
/bin/busybox mount -t sysfs sysfs /sys
/bin/busybox mount -t devtmpfs dev /dev 2>/dev/null
/bin/busybox --install -s /bin 2>/dev/null
exec > /dev/console 2>&1
root=
i=0; while [ $i -lt 60 ]; do
    for d in /dev/mmcblk0p2 /dev/mmcblk1p2 /dev/mmcblk2p2; do
        [ -e "$d" ] && { root="$d"; break; }
    done
    [ -n "$root" ] && break
    usleep 500000; i=$((i+1))
done
[ -n "$root" ] || { echo "WESTON-PREINIT: no rootfs partition found"; exec sh; }
mkdir -p /mnt
mount -t ext4 "$root" /mnt || { echo "WESTON-PREINIT: mount $root failed"; exec sh; }
ini=/mnt/etc/xdg/weston/weston.ini
if [ -f "$ini" ]; then
    sed -i 's/^use-g2d=true/use-g2d=false/' "$ini"
    grep -q '^renderer=' "$ini" || sed -i 's/^use-g2d=false/use-g2d=false\nrenderer=pixman/' "$ini"
    grep -q '^renderer=' "$ini" || printf '\n[core]\nrenderer=pixman\nuse-g2d=false\n' >> "$ini"
    echo "WESTON-PREINIT: rootfs=$root $(grep -E 'use-g2d|renderer' "$ini" | tr '\n' ' ')"
else
    printf '[core]\nrenderer=pixman\nidle-time=0\n' > "$ini" 2>/dev/null || true
    echo "WESTON-PREINIT: rootfs=$root (wrote fresh weston.ini -> pixman)"
fi
exec switch_root /mnt /sbin/init
INIT
chmod +x "$STAGE/init"
( cd "$STAGE" && find . | cpio -o -H newc 2>/dev/null | gzip ) > "$WORK/initrd.cpio.gz"

# --- Boot + capture. ---------------------------------------------------------
"$QEMU" -M imx93-11x11-evk -audio driver=none -m 4G -display none \
    -qmp "unix:$QMP,server=on,wait=off" \
    -kernel "$KERNEL" -dtb "$WORK/panel.dtb" -initrd "$WORK/initrd.cpio.gz" \
    -append "console=ttyLP0,115200 cpuidle.off=1 rdinit=/init systemd.log_target=console systemd.journald.forward_to_console=1" \
    -drive if=sd,format=raw,file="$DISK" \
    -serial file:"$LOG" -serial null >/dev/null 2>&1 &
QPID=$!

shot() { python3 - "$QMP" "$OUT" <<'PY' 2>/dev/null
import socket, json, sys
try:
    s=socket.socket(socket.AF_UNIX); s.connect(sys.argv[1]); f=s.makefile("rw"); f.readline()
    def c(o): f.write(json.dumps(o)+"\r\n"); f.flush(); return f.readline()
    c({"execute":"qmp_capabilities"})
    c({"execute":"screendump","arguments":{"filename":sys.argv[2],"format":"png"}})
except Exception:
    pass
PY
}
painted() { python3 - "$OUT" <<'PY' 2>/dev/null
import sys
from PIL import Image, ImageStat
im=Image.open(sys.argv[1]).convert("L"); st=ImageStat.Stat(im)
print(f"{im.width}x{im.height} mean={st.mean[0]:.1f} std={st.stddev[0]:.1f}")
sys.exit(0 if st.stddev[0] > 8 else 1)
PY
}

ok=0; start=$(date +%s)
while [ $(( $(date +%s) - start )) -lt "$TMO" ]; do
    kill -0 "$QPID" 2>/dev/null || break
    sleep 20; el=$(( $(date +%s) - start ))
    mk=0; grep -qaE 'Started.*Wayland compositor' "$LOG" 2>/dev/null && mk=1
    { [ "$mk" = 1 ] || [ "$el" -gt 600 ]; } || continue
    sleep 8; shot
    info=$(painted) && { echo "  ok   weston desktop painted [$info] (marker=$mk)"; ok=1; break; }
done
shot
kill "$QPID" 2>/dev/null || true; QPID=""

echo "=== weston report ==="
grep -aE 'WESTON-PREINIT|Started.*Wayland compositor|No mali|weston.service.*[Ff]ail' "$LOG" 2>/dev/null | tail -5
if [ "$ok" = 1 ]; then
    cp "$OUT" "${WESTON_PNG:-$ROOT/build-imx93/weston-desktop.png}" 2>/dev/null || true
    echo "PASS: Weston desktop up on the LCDIFv3/$PANEL panel ($OUT)"; exit 0
fi
echo "FAIL: Weston desktop not confirmed within ${TMO}s ($(painted 2>/dev/null))"; exit 1
