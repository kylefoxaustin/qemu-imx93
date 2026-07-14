#!/bin/sh
#
# i.MX 93 <-> i.MX 93 SPI board-to-board interconnect.
#
# Boots two imx93-11x11-evk guests. Each board's LPSPI1 (spi@44360000) master
# gets a `spi-link` SSI peripheral attached (-device spi-link,bus=lpspi1), and
# the two spi-links are joined by a unix chardev socket - one QEMU listens, the
# other connects. When a master clocks a byte out (MOSI) the spi-link forwards
# it over the socket to the peer; the byte clocked in (MISO) comes from a FIFO
# fed by the peer. So the SENDER clocks the payload out of its master, and the
# RECEIVER clocks dummy bytes to shift the peer's payload in - the data path of
# a real board-to-board SPI link.
#
# LPSPI1 is disabled in the stock EVK dtb, so we flip it to "okay", drop its
# dmas= (the transfers are small PIO), and add a spidev@0 slave so Linux binds
# /dev/spidevN.0. Mirrors the i.MX 91 harness (tests/interconnect-imx91).
set -u

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
QEMU=${QEMU:-$ROOT/build-imx93/qemu-system-aarch64}
[ -x "$QEMU" ] || QEMU=$ROOT/build/qemu-system-aarch64
DEPLOY=${DEPLOY:-$HOME/Documents/nxp/linux/imx-yocto-bsp/build-imx93/tmp/deploy/images/imx93evk}
IMAGE=${IMAGE:-${KERNEL:-$DEPLOY/Image}}
DTB=${DTB:-$DEPLOY/imx93-11x11-evk.dtb}
DTC=${DTC:-$HOME/Documents/nxp/linux/imx-yocto-bsp/build-imx93/tmp/work-shared/imx93evk/kernel-build-artifacts/scripts/dtc/dtc}
INITRD_SRC=${INITRD_SRC:-$ROOT/tests/busybox-initramfs/busybox-initramfs.cpio.gz}
CROSS=${CROSS:-aarch64-linux-gnu-gcc}
PAYLOAD=${PAYLOAD:-IMX93-SPI-LINK-payload-0123456789}
MEM=${MEM:-2G}
TMO=${TMO:-200}

skip() { echo "SKIP: $*"; exit 0; }
die()  { echo "FAIL: $*"; exit 1; }

for f in "$QEMU" "$IMAGE" "$DTB" "$INITRD_SRC"; do [ -e "$f" ] || skip "missing $f"; done
[ -x "$DTC" ] || skip "no dtc (need it to enable lpspi1); set DTC="
command -v "$CROSS" >/dev/null || skip "no cross compiler ($CROSS)"

SOCK=${SOCK:-$(mktemp -u /tmp/imx93-spilink.XXXXXX.sock)}
. "$HERE/reap.sh"
WORK=$(mktemp -d); trap 'reap_all; rm -rf "$WORK" "$SOCK"' EXIT INT TERM HUP
SPID=; CPID=

"$CROSS" -O2 -static -o "$WORK/spilink" "$HERE/spilink.c" || die "spilink build failed"

# Enable LPSPI1 (spi@44360000): status okay, drop dmas, add a spidev slave.
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
DTB2="$WORK/spi.dtb"

build_initrd() {            # $1=role  -> echoes path
    local role=$1
    local stage="$WORK/$role"
    mkdir -p "$stage"
    zcat "$INITRD_SRC" | (cd "$stage" && cpio -idmu 2>/dev/null)
    install -m755 "$WORK/spilink" "$stage/spilink"
    printf 'ROLE=%s\nPAYLOAD=%s\n' "$role" "$PAYLOAD" > "$stage/linkenv"
    cat > "$stage/init" <<'INIT'
#!/bin/busybox sh
/bin/busybox mount -t proc proc /proc
/bin/busybox mount -t devtmpfs dev /dev 2>/dev/null
/bin/busybox mount -t sysfs sys /sys 2>/dev/null
/bin/busybox --install -s /bin 2>/dev/null
exec > /dev/console 2>&1
. /linkenv
/bin/busybox sleep 1
S=$(ls /dev/spidev* 2>/dev/null | head -1)
[ -c "$S" ] || { echo "SPILINK:FAIL:no /dev/spidev (lpspi1 not bound)"; \
                 dmesg | grep -iE 'spi|lpspi' | tail -5; busybox poweroff -f; }
echo "=== INTERCONNECT spi ($ROLE on $S) ==="
if [ "$ROLE" = recv ]; then
    /spilink recv "$S" "$PAYLOAD"
else
    sleep 8                       # let the receiver open + the socket connect,
    n=0                           # then resend across the receiver's clock window
    while [ "$n" -lt 15 ]; do     # (the peer boots slower; generous overlap)
        /spilink send "$S" "$PAYLOAD"
        sleep 2
        n=$((n + 1))
    done
fi
echo "=== INTERCONNECT-DONE ==="
/bin/busybox poweroff -f
INIT
    chmod +x "$stage/init"
    ( cd "$stage" && find . | cpio -o -H newc 2>/dev/null | gzip ) > "$WORK/$role.gz"
    echo "$WORK/$role.gz"
}

RECV_IRD=$(build_initrd recv)
SEND_IRD=$(build_initrd send)

boot() {                    # $1=initrd  $2=chardev-args  $3=logfile
    timeout -k 5 "$TMO" "$QEMU" -M imx93-11x11-evk -audio driver=none -smp 3 -m "$MEM" -display none \
        -kernel "$IMAGE" -dtb "$DTB2" -initrd "$1" \
        -append "console=ttyLP0,115200 cpuidle.off=1 rdinit=/init" \
        $2 -device spi-link,bus=lpspi1,chardev=spil \
        -serial file:"$3" -nic none \
        >/dev/null 2>&1 &
}

RLOG="$WORK/recv.log"; SLOG="$WORK/send.log"
echo "== booting i.MX 93 RECEIVER (spi-link socket listen) =="
boot "$RECV_IRD" "-chardev socket,id=spil,path=$SOCK,server=on,wait=off" "$RLOG"; SPID=$!; reap_track $SPID
sleep 2
echo "== booting i.MX 93 SENDER (spi-link socket connect) =="
boot "$SEND_IRD" "-chardev socket,id=spil,path=$SOCK,server=off,reconnect-ms=1000" "$SLOG"; CPID=$!; reap_track $CPID

wait $SPID $CPID 2>/dev/null

echo "================== SPI LINK =================="
grep -aE 'INTERCONNECT|SPILINK:' "$RLOG" "$SLOG" | grep -avE '^\[ *[0-9]+\.[0-9]+\]' | sed 's#.*/##'
if grep -aq 'SPILINK:PASS' "$RLOG"; then
    echo "PASS: payload crossed LPSPI<->spi-link<->socket<->spi-link<->LPSPI byte-exact between two i.MX 93 guests"
    exit 0
fi
die "spi link did not complete"
