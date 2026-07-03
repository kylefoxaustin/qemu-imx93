#!/bin/sh
#
# i.MX 93 <-> i.MX 93 Ethernet board-to-board interconnect.
#
# Boots two imx93-11x11-evk guests, each board's FEC (eth0) bridged by a QEMU
# socket netdev - one instance listens, the other connects. Static IPs on eth0
# (server 192.168.7.1, client 192.168.7.2); the second NIC (-nic user = eQOS/
# eth1) is unused. The client sends a payload over TCP, the server echoes it,
# the client verifies it byte-for-byte: a pass means the payload actually
# traversed guest A -> FEC -> socket bridge -> FEC -> guest B and back.
#
# Mirrors the i.MX 91 harness (tests/interconnect-imx91/run-eth.sh).
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
PORT=${PORT:-12421}
SUBNET=${SUBNET:-192.168.7}
PAYLOAD=${PAYLOAD:-IMX93-ETH-LINK-payload-0123456789-abcdef}
MEM=${MEM:-2G}            # < ~1G starves the FEC DMA coherent pool -> abort
TMO=${TMO:-180}

skip() { echo "SKIP: $*"; exit 0; }
die()  { echo "FAIL: $*"; exit 1; }

for f in "$QEMU" "$IMAGE" "$DTB" "$INITRD_SRC"; do [ -e "$f" ] || skip "missing $f"; done
command -v "$CROSS" >/dev/null || skip "no cross compiler ($CROSS)"

SPID=; CPID=
WORK=$(mktemp -d); trap 'rm -rf "$WORK"; kill ${SPID:-} ${CPID:-} 2>/dev/null' EXIT

"$CROSS" -O2 -static -o "$WORK/linktool" "$HERE/linktool.c" || die "linktool build failed"

build_initrd() {            # $1=role  $2=our-ip  $3=peer-ip  -> echoes path
    local role=$1 myip=$2 peer=$3
    local stage="$WORK/$role"
    mkdir -p "$stage"
    zcat "$INITRD_SRC" | (cd "$stage" && cpio -idmu 2>/dev/null)
    install -m755 "$WORK/linktool" "$stage/linktool"
    printf 'ROLE=%s\nMYIP=%s\nPEER=%s\nPORT=%s\nPAYLOAD=%s\n' \
        "$role" "$myip" "$peer" "$PORT" "$PAYLOAD" > "$stage/linkenv"
    cat > "$stage/init" <<'INIT'
#!/bin/busybox sh
/bin/busybox mount -t proc proc /proc
/bin/busybox mount -t devtmpfs dev /dev 2>/dev/null
/bin/busybox --install -s /bin 2>/dev/null
exec > /dev/console 2>&1
. /linkenv
ifconfig lo 127.0.0.1 up
ifconfig eth0 "$MYIP" netmask 255.255.255.0 up
echo "=== INTERCONNECT eth ($ROLE $MYIP -> peer $PEER) ==="
if [ "$ROLE" = server ]; then
    /linktool server "$PORT"
else
    /linktool client "$PEER" "$PORT" "$PAYLOAD"
fi
echo "=== INTERCONNECT-DONE ==="
/bin/busybox poweroff -f
INIT
    chmod +x "$stage/init"
    ( cd "$stage" && find . | cpio -o -H newc 2>/dev/null | gzip ) > "$WORK/$role.gz"
    echo "$WORK/$role.gz"
}

SRV_IRD=$(build_initrd server "$SUBNET.1" "$SUBNET.2")
CLI_IRD=$(build_initrd client "$SUBNET.2" "$SUBNET.1")

boot() {                    # $1=initrd  $2=nic-arg  $3=logfile
    timeout "$TMO" "$QEMU" -M imx93-11x11-evk -smp 3 -m "$MEM" -display none \
        -kernel "$IMAGE" -dtb "$DTB" -initrd "$1" \
        -append "console=ttyLP0,115200 cpuidle.off=1 rdinit=/init" \
        -nic "$2" -nic user \
        -serial file:"$3" -serial null >/dev/null 2>&1 &
}

SLOG="$WORK/server.log"; CLOG="$WORK/client.log"
echo "== booting i.MX 93 SERVER (socket listen :$PORT) =="
boot "$SRV_IRD" "socket,listen=127.0.0.1:$PORT" "$SLOG"; SPID=$!
sleep 2   # let the listener bind before the connector dials
echo "== booting i.MX 93 CLIENT (socket connect :$PORT) =="
boot "$CLI_IRD" "socket,connect=127.0.0.1:$PORT" "$CLOG"; CPID=$!

wait $SPID $CPID 2>/dev/null

echo "================== ETHERNET LINK =================="
grep -aE 'INTERCONNECT|LINK:' "$SLOG" "$CLOG" | grep -avE '^\[ *[0-9]+\.[0-9]+\]' | sed 's#.*/##'
spass=$(grep -ac 'LINK:PASS:server' "$SLOG"); cpass=$(grep -ac 'LINK:PASS:client' "$CLOG")
if [ "$spass" -ge 1 ] && [ "$cpass" -ge 1 ]; then
    echo "PASS: payload crossed FEC<->socket<->FEC byte-exact between two i.MX 93 guests"
    exit 0
fi
die "eth link did not complete"
