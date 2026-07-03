#!/bin/sh
#
# i.MX 93 <-> i.MX 93 UART board-to-board interconnect.
#
# Boots two imx93-11x11-evk guests and wires each one's LPUART2
# (serial@44390000, /dev/ttyLP1) to a shared unix-domain chardev socket -
# one QEMU listens, the other connects. The sender writes a fixed payload
# to /dev/ttyLP1; the receiver reads it and checks it byte-for-byte. This
# is the real RS-232-style board-to-board link a developer would solder
# between two EVKs, modelled end to end.
#
# LPUART1 (serial@44380000, /dev/ttyLP0) stays the console; LPUART2 is
# disabled in the stock EVK dtb, so we flip its status to "okay" with dtc
# before booting.
#
# Mirrors the i.MX 91 harness (tests/interconnect-imx91/run-uart.sh); the
# two SoCs share the same LPUART2 address and the same fsl-lpuart model.
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
PAYLOAD=${PAYLOAD:-IMX93-UART-LINK-payload-0123456789}
MEM=${MEM:-2G}
TMO=${TMO:-180}

skip() { echo "SKIP: $*"; exit 0; }
die()  { echo "FAIL: $*"; exit 1; }

for f in "$QEMU" "$IMAGE" "$DTB" "$INITRD_SRC"; do [ -e "$f" ] || skip "missing $f"; done
[ -x "$DTC" ] || skip "no dtc (need it to enable LPUART2); set DTC="

SOCK=${SOCK:-$(mktemp -u /tmp/imx93-uartlink.XXXXXX.sock)}
WORK=$(mktemp -d); trap 'rm -rf "$WORK" "$SOCK"; kill ${SPID:-} ${CPID:-} 2>/dev/null' EXIT
SPID=; CPID=

# Enable LPUART2 (serial@44390000) in the dtb: status "disabled" -> "okay".
"$DTC" -I dtb -O dts "$DTB" 2>/dev/null > "$WORK/base.dts" || die "dtc decompile failed"
awk '
  /serial@44390000 \{/ { inn = 1 }
  inn && /status = "disabled"/ { sub(/disabled/, "okay"); inn = 0 }
  { print }
' "$WORK/base.dts" > "$WORK/uart.dts"
"$DTC" -I dts -O dtb "$WORK/uart.dts" 2>/dev/null > "$WORK/uart.dtb" || die "dtc recompile failed"
DTB2="$WORK/uart.dtb"

build_initrd() {            # $1=role  -> echoes path
    local role=$1
    local stage="$WORK/$role"
    mkdir -p "$stage"
    zcat "$INITRD_SRC" | (cd "$stage" && cpio -idmu 2>/dev/null)
    printf 'ROLE=%s\nPAYLOAD=%s\n' "$role" "$PAYLOAD" > "$stage/linkenv"
    cat > "$stage/init" <<'INIT'
#!/bin/busybox sh
/bin/busybox mount -t proc proc /proc
/bin/busybox mount -t devtmpfs dev /dev 2>/dev/null
/bin/busybox --install -s /bin 2>/dev/null
exec > /dev/console 2>&1
. /linkenv
U=/dev/ttyLP1
[ -c "$U" ] || { echo "LINK:FAIL:uart:no $U (LPUART2 not enabled)"; busybox poweroff -f; }
stty -F "$U" raw -echo 115200 2>/dev/null
echo "=== INTERCONNECT uart ($ROLE on $U) ==="
if [ "$ROLE" = recv ]; then
    if read -t 60 RX < "$U"; then
        if [ "$RX" = "$PAYLOAD" ]; then
            echo "LINK:PASS:recv:got byte-exact [$RX]"
        else
            echo "LINK:FAIL:recv:mismatch [$RX] != [$PAYLOAD]"
        fi
    else
        echo "LINK:FAIL:recv:timeout"
    fi
else
    # The 93 boots slower than the 91 (2 A55 + M33), so give the receiver time
    # to reach its read(), then resend a few times - a plain UART has no
    # retransmit, so one missed burst before the peer is listening = a drop.
    sleep 12                      # let the receiver open + the socket connect
    n=0
    while [ "$n" -lt 5 ]; do
        printf '%s\n' "$PAYLOAD" > "$U"
        echo "LINK:SENT:$PAYLOAD"
        sleep 3
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
    timeout "$TMO" "$QEMU" -M imx93-11x11-evk -smp 3 -m "$MEM" -display none \
        -kernel "$IMAGE" -dtb "$DTB2" -initrd "$1" \
        -append "console=ttyLP0,115200 cpuidle.off=1 rdinit=/init" \
        $2 -serial file:"$3" -serial chardev:ul \
        >/dev/null 2>&1 &
}

RLOG="$WORK/recv.log"; SLOG="$WORK/send.log"
echo "== booting i.MX 93 RECEIVER (LPUART2 socket listen) =="
boot "$RECV_IRD" "-chardev socket,id=ul,path=$SOCK,server=on,wait=off" "$RLOG"; SPID=$!
sleep 2
echo "== booting i.MX 93 SENDER (LPUART2 socket connect) =="
boot "$SEND_IRD" "-chardev socket,id=ul,path=$SOCK,server=off" "$SLOG"; CPID=$!

wait $SPID $CPID 2>/dev/null

echo "================== UART LINK =================="
grep -aE 'INTERCONNECT|LINK:' "$RLOG" "$SLOG" | grep -avE '^\[ *[0-9]+\.[0-9]+\]' | sed 's#.*/##'
if grep -aq 'LINK:PASS:recv' "$RLOG"; then
    echo "PASS: payload crossed LPUART2<->socket<->LPUART2 byte-exact between two i.MX 93 guests"
    exit 0
fi
die "uart link did not complete"
