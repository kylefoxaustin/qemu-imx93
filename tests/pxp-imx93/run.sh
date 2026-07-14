#!/usr/bin/env bash
#
# i.MX93 PXP 2D engine end-to-end test: a g2d_copy through the real stack.
#
# Drives libg2d (imx-pxp-g2d) -> /dev/pxp_device -> the built-in pxp_dma_v3
# driver -> the PXP model. The guest runs g2d_copy_test (cross-compiled here,
# staged into a throwaway imx-image-core ext4) which copies one buffer to another
# via the PXP and self-checks the bytes, printing PXP-G2D-COPY: PASS/FAIL on the
# console. Set CAPTURE=1 to also run QEMU with PXP_DBG and dump the PXP register
# trace (used to reverse the driver's blit sequence while modelling the datapath).
#
# Override via env: KERNEL=, DTB=, ROOTFS=, QEMU=, CAPTURE=1, OUT=.
set -u

HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
DEPLOY=${DEPLOY:-$HOME/Documents/nxp/linux/imx-yocto-bsp/build-imx93/tmp/deploy/images/imx93evk}
FEED=${FEED:-$DEPLOY/../../deb}

QEMU=${QEMU:-$REPO/build-imx93/qemu-system-aarch64}
KERNEL=${KERNEL:-$DEPLOY/Image}
DTB=${DTB:-$DEPLOY/imx93-11x11-evk.dtb}
ROOTFS=${ROOTFS:-$DEPLOY/imx-image-core-imx93evk.rootfs.tar.zst}
CAPTURE=${CAPTURE:-0}
OUT=${OUT:-$HERE/build}
CC=${CC:-aarch64-linux-gnu-gcc}

skip() { echo "SKIP: $*" >&2; exit 0; }

[ -x "$QEMU" ] || { echo "error: build qemu-system-aarch64 first ($QEMU)" >&2; exit 1; }
for t in fakeroot zstd dpkg-deb mkfs.ext4 "$CC"; do
    command -v "$t" >/dev/null || skip "host tool '$t' missing"
done
[ -e "$KERNEL" ] || skip "kernel Image not found: $KERNEL (build the BSP)"
[ -e "$DTB" ]    || skip "device tree not found: $DTB"
[ -e "$ROOTFS" ] || skip "imx-image-core rootfs not found (bitbake imx-image-core)"
[ -d "$FEED" ]   || skip "BSP .deb feed not found: $FEED"
ROOTFS=$(readlink -f "$ROOTFS")
mkdir -p "$OUT"

# Cross-compile the oracle against libg2d (headers + .so from the BSP feed).
echo "==> cross-compiling g2d_copy_test"
LK="$OUT/libg2d"; rm -rf "$LK"; mkdir -p "$LK"
dpkg-deb -x "$(ls "$FEED"/*/libg2d2_*.deb | head -1)" "$LK"
dpkg-deb -x "$(ls "$FEED"/*/libg2d-dev_*.deb | head -1)" "$LK" 2>/dev/null
"$CC" -O2 -Wall -o "$OUT/g2d_copy_test" "$HERE/g2d_copy_test.c" \
    -I"$LK/usr/include" -L"$LK/usr/lib" -lg2d || skip "cross-compile failed"

# Build the throwaway rootfs under fakeroot (root-owned tree; same five fixes as
# tests/gstreamer-imx93: fakeroot ownership, ^metadata_csum, fstab override,
# power-of-2 SD size).
IMG="$OUT/pxp-imx93.ext4"
echo "==> building demo rootfs (fakeroot) -> $IMG"
STAGE="$OUT/rootfs" IMG="$IMG" ROOTFS="$ROOTFS" BIN="$OUT/g2d_copy_test" \
fakeroot bash -c '
set -e
rm -rf "$STAGE"; mkdir -p "$STAGE"
zstd -dc < "$ROOTFS" | tar -C "$STAGE" -xf -
install -D -m0755 "$BIN" "$STAGE/usr/bin/g2d_copy_test"
cat > "$STAGE/etc/fstab" <<EOF
proc       /proc        proc    defaults                            0  0
devpts     /dev/pts     devpts  mode=0620,ptmxmode=0666,gid=5       0  0
tmpfs      /run         tmpfs   mode=0755,nodev,nosuid,strictatime  0  0
tmpfs      /var/volatile tmpfs  defaults                            0  0
EOF
cat > "$STAGE/etc/systemd/system/pxp-g2d.service" <<EOF
[Unit]
Description=PXP g2d_copy oracle
After=multi-user.target
[Service]
Type=oneshot
ExecStart=/usr/bin/g2d_copy_test
StandardOutput=journal+console
StandardError=journal+console
[Install]
WantedBy=multi-user.target
EOF
ln -sf ../pxp-g2d.service "$STAGE/etc/systemd/system/multi-user.target.wants/pxp-g2d.service"
need=$(du -sm "$STAGE" | cut -f1); need=$(( need + need/4 + 128 ))
sz=64; while [ "$sz" -lt "$need" ]; do sz=$(( sz * 2 )); done
rm -f "$IMG"
mkfs.ext4 -F -q -L pxpdemo -O ^metadata_csum -d "$STAGE" "$IMG" "${sz}M"
e2fsck -fy "$IMG" >/dev/null 2>&1 || true
' || { echo "error: rootfs build failed" >&2; exit 1; }

APPEND="console=ttyLP0,115200 root=/dev/mmcblk0 rootwait rw cpuidle.off=1"
LOG="$OUT/serial.log"; TRACE="$OUT/pxp-trace.log"
rm -f "$LOG" "$TRACE"
ENV=()
[ "$CAPTURE" = 1 ] && ENV=(env PXP_DBG=1)

echo "==> booting (CAPTURE=$CAPTURE); watching for PXP-G2D-COPY result"
"${ENV[@]}" "$QEMU" -M imx93-11x11-evk -audio driver=none -m 4G \
    -kernel "$KERNEL" -dtb "$DTB" \
    -drive if=sd,file="$IMG",format=raw -append "$APPEND" \
    -display none -serial "file:$LOG" -serial null 2>"$TRACE" &
qpid=$!
trap 'kill $qpid 2>/dev/null' EXIT
for s in $(seq 1 60); do
    sleep 5
    grep -qa "PXP-G2D-ROT270:" "$LOG" 2>/dev/null && break
done
kill $qpid 2>/dev/null; trap - EXIT

ok=1
for op in COPY FILL BLIT BLEND; do
    line=$(grep -a "PXP-G2D-$op:" "$LOG" | head -1)
    echo "==> ${line:-PXP-G2D-$op: (no result)}"
    [ "${line#*PASS}" != "$line" ] || ok=0
done
[ "$CAPTURE" = 1 ] && echo "==> PXP register trace: $TRACE ($(grep -c '\[pxp\]' "$TRACE" 2>/dev/null) accesses)"
[ "$ok" = 1 ] && exit 0
echo "FAIL or missing result; see $LOG" >&2
exit 1
