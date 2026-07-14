#!/bin/sh
#
# i.MX 93 <-> i.MX 93 I2C board-to-board interconnect.
#
# Boots two imx93-11x11-evk guests. Each board's LPI2C3 master gets an i2c-link
# target attached at a fixed address (-device i2c-link,bus=lpi2c3,address=0x42),
# and the two i2c-links are joined by a unix chardev socket - one listens, the
# other connects. The sender's master WRITES a payload to the link address (the
# link forwards it over the socket to the peer); the receiver's master READS
# from the link address (the link returns the bytes the peer wrote) and checks
# them byte-for-byte. i2c-link is the analogue of spi-link for I2C - a bridged
# mailbox, both boards driving their own LPI2C as master.
#
# LPI2C3 is enabled on the stock EVK dtb and i2c-dev is builtin, so no dtb edit
# or module load is needed (unlike SPI/CAN). Mirrors the i.MX 91 harness style.
set -u

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
QEMU=${QEMU:-$ROOT/build-imx93/qemu-system-aarch64}
[ -x "$QEMU" ] || QEMU=$ROOT/build/qemu-system-aarch64
DEPLOY=${DEPLOY:-$HOME/Documents/nxp/linux/imx-yocto-bsp/build-imx93/tmp/deploy/images/imx93evk}
IMAGE=${IMAGE:-${KERNEL:-$DEPLOY/Image}}
DTB=${DTB:-$DEPLOY/imx93-11x11-evk.dtb}
INITRD_SRC=${INITRD_SRC:-$ROOT/tests/busybox-initramfs/busybox-initramfs.cpio.gz}
CROSS=${CROSS:-aarch64-linux-gnu-gcc}
ADDR=${ADDR:-0x42}
PAYLOAD=${PAYLOAD:-IMX93-I2C-LINK-payload-0123456789}
MEM=${MEM:-2G}
TMO=${TMO:-200}

skip() { echo "SKIP: $*"; exit 0; }
die()  { echo "FAIL: $*"; exit 1; }

for f in "$QEMU" "$IMAGE" "$DTB" "$INITRD_SRC"; do [ -e "$f" ] || skip "missing $f"; done
command -v "$CROSS" >/dev/null || skip "no cross compiler ($CROSS)"

SOCK=${SOCK:-$(mktemp -u /tmp/imx93-i2clink.XXXXXX.sock)}
WORK=$(mktemp -d); trap 'rm -rf "$WORK" "$SOCK"; kill ${SPID:-} ${CPID:-} 2>/dev/null' EXIT
SPID=; CPID=

"$CROSS" -O2 -static -o "$WORK/i2clink" "$HERE/i2clink.c" || die "i2clink build failed"

build_initrd() {            # $1=role  -> echoes path
    local role=$1
    local stage="$WORK/$role"
    mkdir -p "$stage"
    zcat "$INITRD_SRC" | (cd "$stage" && cpio -idmu 2>/dev/null)
    install -m755 "$WORK/i2clink" "$stage/i2clink"
    printf 'ROLE=%s\nADDR=%s\nPAYLOAD=%s\n' "$role" "$ADDR" "$PAYLOAD" > "$stage/linkenv"
    cat > "$stage/init" <<'INIT'
#!/bin/busybox sh
/bin/busybox mount -t proc proc /proc
/bin/busybox mount -t sysfs sysfs /sys 2>/dev/null
/bin/busybox mount -t devtmpfs dev /dev 2>/dev/null
/bin/busybox --install -s /bin 2>/dev/null
exec > /dev/console 2>&1
. /linkenv
# find the /dev/i2c-N adapter for LPI2C3 (i2c@42530000)
DEV=""
n=0
while [ -z "$DEV" ] && [ $n -lt 30 ]; do
    for a in /sys/class/i2c-dev/i2c-*; do
        [ -e "$a" ] || continue
        on=$(/bin/busybox readlink "$a/device/of_node" 2>/dev/null)
        case "$on" in *42530000*) DEV="/dev/$(/bin/busybox basename "$a")"; break;; esac
    done
    [ -z "$DEV" ] && { /bin/busybox sleep 1; n=$((n+1)); }
done
[ -c "$DEV" ] || { echo "I2CLINK:FAIL:no /dev/i2c for lpi2c3"; ls /dev/i2c-* 2>/dev/null; busybox poweroff -f; }
echo "=== INTERCONNECT i2c ($ROLE on $DEV @ $ADDR) ==="
if [ "$ROLE" = recv ]; then
    /i2clink recv "$DEV" "$ADDR" "$PAYLOAD"
else
    sleep 8; c=0
    while [ "$c" -lt 15 ]; do /i2clink send "$DEV" "$ADDR" "$PAYLOAD"; sleep 2; c=$((c+1)); done
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
    timeout "$TMO" "$QEMU" -M imx93-11x11-evk -audio driver=none -smp 3 -m "$MEM" -display none \
        -kernel "$IMAGE" -dtb "$DTB" -initrd "$1" \
        -append "console=ttyLP0,115200 cpuidle.off=1 rdinit=/init" \
        $2 -device i2c-link,bus=lpi2c3,address="$ADDR",chardev=i2cl \
        -serial file:"$3" -serial null -monitor none >/dev/null 2>&1 &
}

RLOG="$WORK/recv.log"; SLOG="$WORK/send.log"
echo "== booting i.MX 93 RECEIVER (i2c-link socket listen) =="
boot "$RECV_IRD" "-chardev socket,id=i2cl,path=$SOCK,server=on,wait=off" "$RLOG"; SPID=$!
sleep 2
echo "== booting i.MX 93 SENDER (i2c-link socket connect) =="
boot "$SEND_IRD" "-chardev socket,id=i2cl,path=$SOCK,server=off,reconnect-ms=1000" "$SLOG"; CPID=$!

wait $SPID $CPID 2>/dev/null

echo "================== I2C LINK =================="
grep -aE 'INTERCONNECT|I2CLINK:' "$RLOG" "$SLOG" | grep -avE '^\[ *[0-9]+\.[0-9]+\]' | sed 's#.*/##'
if grep -aq 'I2CLINK:PASS' "$RLOG"; then
    echo "PASS: payload crossed LPI2C<->i2c-link<->socket<->i2c-link<->LPI2C byte-exact between two i.MX 93 guests"
    exit 0
fi
die "i2c link did not complete"
