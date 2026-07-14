#!/bin/sh
#
# i.MX 93 <-> i.MX 93 CAN board-to-board interconnect.
#
# Boots two imx93-11x11-evk guests, each with its FlexCAN on an emulated
# can-bus bridged to a QEMU chardev by can-host-chardev - one instance listens,
# the other connects. The sender transmits a CAN frame (id 0x321, "CANLink!"),
# the receiver reads it and checks it byte-for-byte. A pass means the frame
# actually crossed FlexCAN TX -> can-bus -> can-host-chardev -> socket -> peer
# -> FlexCAN RX. can-host-chardev bridges an emulated can-bus to a chardev (vs
# host SocketCAN), so no vcan/root is needed. Mirrors the i.MX 95 harness.
#
# FlexCAN is disabled in the stock EVK dtb, so a dt overlay (tests/flexcan)
# enables flexcan1/2 + a dummy transceiver regulator; both nodes wire to one
# can-bus (Linux may enumerate either as can0). CAN is a set of kernel modules,
# loaded from the BSP rootfs.
set -u

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
FLEX="$ROOT/tests/flexcan"
QEMU=${QEMU:-$ROOT/build-imx93/qemu-system-aarch64}
[ -x "$QEMU" ] || QEMU=$ROOT/build/qemu-system-aarch64
DEPLOY=${DEPLOY:-$HOME/Documents/nxp/linux/imx-yocto-bsp/build-imx93/tmp/deploy/images/imx93evk}
IMAGE=${IMAGE:-${KERNEL:-$DEPLOY/Image}}
DTB=${DTB:-$DEPLOY/imx93-11x11-evk.dtb}
KDTC=${KDTC:-$HOME/Documents/nxp/linux/imx-yocto-bsp/build-imx93/tmp/work-shared/imx93evk/kernel-build-artifacts/scripts/dtc}
DTC=${DTC:-$KDTC/dtc}
FDTOVERLAY=${FDTOVERLAY:-$KDTC/fdtoverlay}
MODDIR=${MODDIR:-$ROOT/tests/gstreamer-imx93/build/rootfs/usr/lib/modules/6.12.49-lts-next-gdf24f9428e38}
INITRD_SRC=${INITRD_SRC:-$ROOT/tests/busybox-initramfs/busybox-initramfs.cpio.gz}
CROSS=${CROSS:-aarch64-linux-gnu-gcc}
MEM=${MEM:-2G}
TMO=${TMO:-200}

CAN_KOS="kernel/net/can/can.ko kernel/drivers/net/can/dev/can-dev.ko kernel/net/can/can-raw.ko kernel/drivers/net/can/flexcan/flexcan.ko"

skip() { echo "SKIP: $*"; exit 0; }
die()  { echo "FAIL: $*"; exit 1; }

for f in "$QEMU" "$IMAGE" "$DTC" "$FDTOVERLAY" "$INITRD_SRC" "$DTB" \
         "$FLEX/flexcan-overlay.dtso"; do
    [ -e "$f" ] || skip "missing $f"
done
command -v "$CROSS" >/dev/null || skip "no cross compiler ($CROSS)"
for ko in $CAN_KOS; do [ -e "$MODDIR/$ko" ] || skip "missing CAN module $ko"; done

SOCK=${SOCK:-$(mktemp -u /tmp/imx93-canlink.XXXXXX.sock)}
WORK=$(mktemp -d); trap 'rm -rf "$WORK" "$SOCK"; kill ${SPID:-} ${CPID:-} 2>/dev/null' EXIT
SPID=; CPID=

"$CROSS" -O2 -static -o "$WORK/canlink" "$HERE/canlink.c" || die "canlink build failed"
"$DTC" -@ -I dts -O dtb -o "$WORK/ov.dtbo" "$FLEX/flexcan-overlay.dtso" 2>/dev/null \
    || die "overlay compile failed"
"$FDTOVERLAY" -i "$DTB" -o "$WORK/can.dtb" "$WORK/ov.dtbo" || die "fdtoverlay failed"
DTB2="$WORK/can.dtb"

build_initrd() {            # $1=role  -> echoes path
    local role=$1
    local stage="$WORK/$role"
    mkdir -p "$stage/mod"
    zcat "$INITRD_SRC" | (cd "$stage" && cpio -idmu 2>/dev/null)
    install -m755 "$WORK/canlink" "$stage/canlink"
    for ko in $CAN_KOS; do cp "$MODDIR/$ko" "$stage/mod/$(basename "$ko")"; done
    printf 'ROLE=%s\n' "$role" > "$stage/linkenv"
    cat > "$stage/init" <<'INIT'
#!/bin/busybox sh
/bin/busybox mount -t proc proc /proc
/bin/busybox mount -t sysfs sysfs /sys 2>/dev/null
/bin/busybox mount -t devtmpfs dev /dev 2>/dev/null
/bin/busybox --install -s /bin 2>/dev/null
exec > /dev/console 2>&1
. /linkenv
for m in can can-dev can-raw flexcan; do insmod /mod/$m.ko 2>/dev/null; done
n=0; while [ ! -e /sys/class/net/can0 ] && [ $n -lt 30 ]; do sleep 1; n=$((n+1)); done
[ -e /sys/class/net/can0 ] || { echo "CANLINK:FAIL:no can0 (flexcan not bound)"; \
                                dmesg | grep -iE 'can|flexcan' | tail -5; busybox poweroff -f; }
echo "=== INTERCONNECT can ($ROLE on can0) ==="
if [ "$ROLE" = recv ]; then
    /canlink recv can0
else
    sleep 8                       # let the receiver bring up + the socket connect
    /canlink send can0 15         # then resend across the receiver's window
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
    timeout --signal=KILL "$TMO" "$QEMU" -M imx93-11x11-evk -audio driver=none -smp 3 -m "$MEM" -display none \
        -kernel "$IMAGE" -dtb "$DTB2" -initrd "$1" \
        -append "console=ttyLP0,115200 cpuidle.off=1 rdinit=/init" \
        -object can-bus,id=cb -machine canbus0=cb,canbus1=cb \
        $2 -object can-host-chardev,id=canh,canbus=cb,chardev=canl \
        -serial file:"$3" -serial null -monitor none >/dev/null 2>&1 &
}

RLOG="$WORK/recv.log"; SLOG="$WORK/send.log"
echo "== booting i.MX 93 RECEIVER (can-host-chardev socket listen) =="
boot "$RECV_IRD" "-chardev socket,id=canl,path=$SOCK,server=on,wait=off" "$RLOG"; SPID=$!
sleep 3
echo "== booting i.MX 93 SENDER (can-host-chardev socket connect) =="
boot "$SEND_IRD" "-chardev socket,id=canl,path=$SOCK,server=off,reconnect-ms=1000" "$SLOG"; CPID=$!

wait $SPID $CPID 2>/dev/null

echo "================== CAN LINK =================="
grep -aE 'INTERCONNECT|CANLINK:' "$RLOG" "$SLOG" | grep -avE '^\[ *[0-9]+\.[0-9]+\]' | \
    sed 's/\x1b\[[0-9;]*[a-zA-Z]//g; s#.*/##'
if grep -aq 'CANLINK:PASS' "$RLOG"; then
    echo "PASS: CAN frame crossed FlexCAN<->can-bus<->can-host-chardev<->socket<->FlexCAN byte-exact between two i.MX 93 guests"
    exit 0
fi
die "can link did not complete"
