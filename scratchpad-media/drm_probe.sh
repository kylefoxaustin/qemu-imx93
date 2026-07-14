#!/usr/bin/env bash
# Quick probe: boot the rm67199 display DTB with our cross-built modetest and
# dump the DRM/KMS topology (connectors/CRTCs/encoders/planes/modes) so we can
# design the display conformance cases. Also validates modetest runs in-guest.
set -u
DEPLOY=/home/kyle/Documents/nxp/linux/imx-yocto-bsp/build-imx93/tmp/deploy/images/imx93evk
QEMU=/home/kyle/Documents/GitHub/93emulator/build-imx93/qemu-system-aarch64
KERNEL=$DEPLOY/Image
DTB=$DEPLOY/imx93-11x11-evk-rm67199.dtb
BASE=/home/kyle/Documents/nxp/imx93-initramfs.cpio.gz
MT=/home/kyle/Documents/GitHub/93emulator/scratchpad-media/build/modetest
OUT=/home/kyle/Documents/GitHub/93emulator/scratchpad-media/drm_probe.log
: > "$OUT"
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
cp "$MT" "$TMP/modetest"
cat > "$TMP/myinit" <<'EOF'
#!/bin/sh
PATH=/sbin:/usr/sbin:/bin:/usr/bin; export PATH
mount -t proc proc /proc; mount -t sysfs sysfs /sys; mount -t devtmpfs devtmpfs /dev
sleep 4
echo "=== DRM PROBE ==="
echo "--- /dev/dri ---"; ls -l /dev/dri/ 2>&1
echo "--- drm sysfs status/modes ---"
for c in /sys/class/drm/card*-*/status; do echo "$c = $(cat $c 2>/dev/null)"; done
for m in /sys/class/drm/card*-*/modes; do echo "$m:"; cat "$m" 2>/dev/null | head -4; done
echo "--- modetest (resources) ---"
/modetest 2>&1 | head -120
echo "--- modetest -M imx-drm ---"
/modetest -M imx-drm 2>&1 | head -10
echo "=== DRM PROBE DONE ==="
while true; do sleep 5; done
EOF
chmod +x "$TMP/myinit"
( cd "$TMP" && printf 'myinit\nmodetest\n' | cpio -o -H newc 2>/dev/null > o.cpio )
cat "$BASE" "$TMP/o.cpio" > "$TMP/c.cpio.gz"
timeout -s KILL 180 "$QEMU" -M imx93-11x11-evk -audio driver=none -m 4G -display none \
  -kernel "$KERNEL" -dtb "$DTB" -initrd "$TMP/c.cpio.gz" \
  -append "console=ttyLP0,115200 cpuidle.off=1 rdinit=/myinit ignore_loglevel" \
  -serial "file:$OUT" -serial null &
QP=$!
for _ in $(seq 1 180); do
  sleep 1
  grep -q "DRM PROBE DONE" "$OUT" 2>/dev/null && break
  kill -0 $QP 2>/dev/null || break
done
kill -9 $QP 2>/dev/null; wait $QP 2>/dev/null
echo "=== PROBE OUTPUT ==="
sed -n '/=== DRM PROBE ===/,/=== DRM PROBE DONE ===/p' "$OUT"
