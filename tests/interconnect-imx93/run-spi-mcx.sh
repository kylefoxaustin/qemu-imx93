#!/bin/sh
#
# i.MX 93 <-> MCXN947 cross-SoC SPI board-to-board (shared spi_link.c transport).
#
# Proves the fleet's spi-link SSI-to-chardev bridge interoperates between a
# Linux fsl-lpspi master (this i.MX 93, over /dev/spidev) and a bare-metal
# Cortex-M33 master (the MCXN947, tests/mcxn-spi-link firmware). Both attach a
# spi-link to their named LPSPI SSI bus, joined by a unix socket - the MCX end
# listens, the 93 end connects. The 93 clocks a [0xA5, 32-byte pattern] frame
# out (MOSI -> the MCX collects + verifies it) while draining the MCX's 0x5A
# stream in (MISO -> we verify it). Byte-exact both directions.
#
# Cross-repo: needs a built MCXN947 QEMU + arm-none-eabi-gcc for the M33
# firmware. Set RMCX= to the mcxn947qemu checkout; SKIPs cleanly if absent.
set -u

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
QEMU93=${QEMU93:-$ROOT/build-imx93/qemu-system-aarch64}
[ -x "$QEMU93" ] || QEMU93=$ROOT/build/qemu-system-aarch64
RMCX=${RMCX:-$ROOT/../mcxn947qemu}
QEMUMCX=${QEMUMCX:-$RMCX/build/qemu-system-arm}
MCXFW_SRC=${MCXFW_SRC:-$RMCX/tests/mcxn-spi-link}
DEPLOY=${DEPLOY:-$HOME/Documents/nxp/linux/imx-yocto-bsp/build-imx93/tmp/deploy/images/imx93evk}
IMAGE=${IMAGE:-${KERNEL:-$DEPLOY/Image}}
DTB=${DTB:-$DEPLOY/imx93-11x11-evk.dtb}
DTC=${DTC:-$HOME/Documents/nxp/linux/imx-yocto-bsp/build-imx93/tmp/work-shared/imx93evk/kernel-build-artifacts/scripts/dtc/dtc}
INITRD_SRC=${INITRD_SRC:-$ROOT/tests/busybox-initramfs/busybox-initramfs.cpio.gz}
CROSS=${CROSS:-aarch64-linux-gnu-gcc}
MCXCC=${MCXCC:-arm-none-eabi-gcc}
MEM=${MEM:-2G}
TMO=${TMO:-120}

skip() { echo "SKIP: $*"; exit 0; }
die()  { echo "FAIL: $*"; exit 1; }

for f in "$QEMU93" "$IMAGE" "$DTB" "$INITRD_SRC" "$QEMUMCX" "$MCXFW_SRC/main.c"; do
    [ -e "$f" ] || skip "missing $f (set RMCX= to the mcxn947qemu checkout?)"
done
[ -x "$DTC" ] || skip "no dtc (need it to enable lpspi1); set DTC="
command -v "$CROSS" >/dev/null || skip "no cross compiler ($CROSS)"
command -v "$MCXCC" >/dev/null || skip "no M33 compiler ($MCXCC) for the MCX firmware"

# ---- peer-artifact provenance -------------------------------------------
#
# A path in a live worktree is not an artifact. We consume the MCX QEMU binary
# and the M33 firmware source out of a peer checkout we neither own nor build,
# so a PASS here can be about bytes that exist in no commit and that the peer's
# next build overwrites. We cannot obey "never test a binary you did not just
# build" - we never build theirs - so we obey its dual: never run against an
# artifact whose provenance we did not verify.
#
# Refuse rather than warn. A warning printed above a green result is a warning
# nobody reads.
#
PEER_SHA=unknown
PEER_DIRTY=no
if git -C "$RMCX" rev-parse --git-dir >/dev/null 2>&1; then
    PEER_SHA=$(git -C "$RMCX" rev-parse --short HEAD 2>/dev/null || echo unknown)
    [ -z "$(git -C "$RMCX" status --porcelain 2>/dev/null)" ] || PEER_DIRTY=yes
fi
PEER_MD5=$(md5sum "$QEMUMCX" 2>/dev/null | cut -d' ' -f1)

# A binary older than the sources it was built from tests code the peer has
# already changed - the stale-binary bug, one repo over. That is never useful.
if [ -n "$(find "$RMCX/hw" "$RMCX/include" -type f \( -name '*.c' -o -name '*.h' \) \
             -newer "$QEMUMCX" -print -quit 2>/dev/null)" ]; then
    die "peer QEMU is STALE: $QEMUMCX is older than sources in $RMCX.
     The peer edited code and did not rebuild, so this run would test bytes
     they have already replaced. Rebuild the peer, or set QEMUMCX= explicitly."
fi

# A dirty peer worktree means the binary and firmware we are about to run exist
# in no commit: the result cannot be attributed to anything. Allow it only if
# the caller says so out loud, and stamp the result when they do.
PROVENANCE="peer=$PEER_SHA md5=$PEER_MD5"
if [ "$PEER_DIRTY" = yes ]; then
    if [ "${ALLOW_UNPROVENANCED_PEER:-0}" != 1 ]; then
        die "peer worktree is DIRTY at $PEER_SHA ($RMCX).
     The MCX QEMU and M33 firmware this run would use exist in no commit, so a
     PASS could not be attributed to any peer state. Commit the peer, or set
     ALLOW_UNPROVENANCED_PEER=1 to run anyway (the result will say so)."
    fi
    PROVENANCE="$PROVENANCE DIRTY(UNPROVENANCED)"
fi
echo "peer artifact: $PROVENANCE"

SOCK=$(mktemp -u /tmp/imx93-mcx-spi.XXXXXX.sock)
WORK=$(mktemp -d)
MPID=
trap 'kill $MPID 2>/dev/null; rm -rf "$WORK" "$SOCK"' EXIT

# Build the 93 spidev peer + the MCX M33 firmware.
"$CROSS" -O2 -static -o "$WORK/spi_peer" "$HERE/spi_peer.c" || die "spi_peer build failed"
"$MCXCC" -mcpu=cortex-m33 -mthumb -nostdlib -nostartfiles -ffreestanding -O2 -Wall \
    -T "$MCXFW_SRC/link.ld" "$MCXFW_SRC/main.c" -o "$WORK/spilink.elf" \
    || die "MCX firmware build failed"

# 93 dtb: enable lpspi1 (spi@44360000), drop dmas, add a spidev@0 slave.
"$DTC" -I dtb -O dts "$DTB" 2>/dev/null > "$WORK/base.dts" || die "dtc decompile failed"
awk '
  /spi@44360000 \{/ { inn = 1 }
  inn && /dmas =|dma-names =/ { next }
  inn && /status = "disabled"/ { print "\t\t\t\tstatus = \"okay\";"; next }
  inn && /^\t\t\t\};/ {
    print "\t\t\t\tspidev@0 {";
    print "\t\t\t\t\tcompatible = \"rohm,dh2228fv\";";
    print "\t\t\t\t\treg = <0x00>;";
    print "\t\t\t\t\tspi-max-frequency = <0xf4240>;";
    print "\t\t\t\t};";
    print; inn = 0; next
  }
  { print }
' "$WORK/base.dts" > "$WORK/spi.dts"
"$DTC" -I dts -O dtb "$WORK/spi.dts" 2>/dev/null > "$WORK/spi.dtb" || die "dtc recompile failed"

# 93 initrd: run the spidev peer against /dev/spidev.
S="$WORK/stage"; mkdir -p "$S"
zcat "$INITRD_SRC" | (cd "$S" && cpio -idmu 2>/dev/null)
install -m755 "$WORK/spi_peer" "$S/spi_peer"
cat > "$S/init" <<'INIT'
#!/bin/busybox sh
/bin/busybox mount -t proc proc /proc
/bin/busybox mount -t devtmpfs dev /dev 2>/dev/null
/bin/busybox mount -t sysfs sys /sys 2>/dev/null
/bin/busybox --install -s /bin 2>/dev/null
exec > /dev/console 2>&1
sleep 1
SP=$(ls /dev/spidev* 2>/dev/null | head -1)
[ -c "$SP" ] || { echo "SPIPEER:FAIL:no /dev/spidev (lpspi1 not bound)"; busybox poweroff -f; }
echo "=== INTERCONNECT spi 93<->MCX ($SP) ==="
/spi_peer "$SP"
echo "=== INTERCONNECT-DONE ==="
/bin/busybox poweroff -f
INIT
chmod +x "$S/init"
( cd "$S" && find . | cpio -o -H newc 2>/dev/null | gzip ) > "$WORK/peer.gz"

MCXCON="$WORK/mcx-console.log"; CLOG="$WORK/93-client.log"

echo "== launching MCXN947 SPI node (bare-metal M33, spi-link socket listen) =="
"$QEMUMCX" -M frdm-mcxn947 -display none -monitor none -serial "file:$MCXCON" \
    -chardev "socket,id=spil,path=$SOCK,server=on,wait=off" \
    -device spi-link,bus=mcxn-lpspi,chardev=spil \
    -kernel "$WORK/spilink.elf" -no-reboot >/dev/null 2>&1 &
MPID=$!
i=0; while [ $i -lt 30 ]; do ss -xl 2>/dev/null | grep -qF "$SOCK" && break; sleep 0.5; i=$((i + 1)); done

echo "== booting i.MX 93 client (Linux fsl-lpspi /dev/spidev, spi-link socket connect) =="
timeout "$TMO" "$QEMU93" -M imx93-11x11-evk -audio driver=none -smp 3 -m "$MEM" -display none \
    -kernel "$IMAGE" -dtb "$WORK/spi.dtb" -initrd "$WORK/peer.gz" \
    -append "console=ttyLP0,115200 cpuidle.off=1 rdinit=/init" \
    -chardev "socket,id=spil,path=$SOCK,server=off,reconnect-ms=1000" \
    -device spi-link,bus=lpspi1,chardev=spil \
    -serial "file:$CLOG" -serial null >/dev/null 2>&1

echo "================== 93 <-> MCX SPI LINK =================="
echo "--- i.MX 93 (Linux fsl-lpspi) ---"; grep -aE 'SPIPEER' "$CLOG" | grep -avE '^\[' | tail -2
echo "--- MCXN947 (bare-metal M33) ---"; grep -aiE 'SPI LINK' "$MCXCON" | tail -2
if grep -aq 'SPIPEER:RXOK' "$CLOG" && grep -aiq 'SPI LINK PASS' "$MCXCON"; then
    echo "PASS: i.MX 93 Linux fsl-lpspi <-> MCXN947 bare-metal M33, byte-exact both directions over the shared spi-link [$PROVENANCE]"
    exit 0
fi
die "cross-SoC spi link did not complete"
