#!/bin/sh
#
# i.MX 93 node for the fleet's cross-silicon raw-L2 segment (ethertype 0x88B9).
#
# Boots an imx93-11x11-evk guest and runs enet-lab3 (the fleet's ratified v2
# beacon/checker) over the 93's real FEC (eth0). Three modes:
#
#   run.sh selftest            Two 93 guests (0x88B9 <-> 0x88BA) cross-wired by a
#                              QEMU socket bridge, each REQUIRING the other. Proves
#                              our emitter + checker + FEC datapath end to end, and
#                              that each boot picks a FRESH per-boot incarnation
#                              (a reboot re-baselines, is never condemned as replay).
#
#   run.sh wireshape           One 93 guest; a host-side validator (independent of
#                              the guest's own checker) captures the beacons over the
#                              SAME udp socket wiring used to attach to rt1180's switch
#                              and asserts every wire field, AND boots twice to prove
#                              the incarnation nonce differs across boots.
#
#   run.sh attach '<nic>' <my_et> <peer_et>...   One 93 guest, FEC wired to <nic>
#                              (e.g. a switch wire-port
#                              'socket,udp=127.0.0.1:45032,localaddr=127.0.0.1:45031'
#                              or the mcast hub), watching the given peers. Reports
#                              exactly what 0x88B9 logs - PASS/CORRUPT/LEGACY - the
#                              assertion, not the appearance.
#
# Mirrors tests/interconnect-imx93/run-eth.sh (FEC over a QEMU socket) and
# 91emulator's tests/interconnect-imx91/run-enet-lab.sh.
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
MEM=${MEM:-2G}                 # < ~1G starves the FEC DMA coherent pool -> abort
WINDOW=${WINDOW:-30}           # seconds the beacon runs inside the guest
TMO=${TMO:-120}
MODE=${1:-selftest}

skip() { echo "SKIP: $*"; exit 0; }
die()  { echo "FAIL: $*"; exit 1; }

for f in "$QEMU" "$IMAGE" "$DTB" "$INITRD_SRC"; do [ -e "$f" ] || skip "missing $f"; done
command -v "$CROSS" >/dev/null || skip "no cross compiler ($CROSS)"

SPID=; CPID=
. "$ROOT/tests/interconnect-imx93/reap.sh"
WORK=$(mktemp -d); trap 'reap_all; rm -rf "$WORK"' EXIT INT TERM HUP

"$CROSS" -O2 -static -o "$WORK/enet-lab3" "$HERE/enet-lab3.c" || die "beacon build failed"
echo "node source md5 : $(md5sum "$HERE/enet-lab3.c" | cut -d' ' -f1)  (tests/enet-lab3/enet-lab3.c)"
echo "static bin  md5 : $(md5sum "$WORK/enet-lab3"  | cut -d' ' -f1)"

# $1=my_et  $2..=peer_ets  -> echoes an initrd path that runs the beacon on eth0
build_initrd() {
    myet=$1; shift; peers="$*"
    stage="$WORK/n$myet"
    mkdir -p "$stage"
    zcat "$INITRD_SRC" | (cd "$stage" && cpio -idmu 2>/dev/null)
    install -m755 "$WORK/enet-lab3" "$stage/enet-lab3"
    printf 'MYET=%s\nPEERS=%s\nWINDOW=%s\n' "$myet" "$peers" "$WINDOW" > "$stage/labenv"
    cat > "$stage/init" <<'INIT'
#!/bin/busybox sh
/bin/busybox mount -t proc proc /proc
/bin/busybox mount -t devtmpfs dev /dev 2>/dev/null
/bin/busybox --install -s /bin 2>/dev/null
exec > /dev/console 2>&1
. /labenv
ifconfig lo 127.0.0.1 up
ifconfig eth0 up            # raw AF_PACKET beacon: link up, no IP needed
echo "=== ENET-LAB3 node up (my_et=$MYET peers=$PEERS) ==="
LAB_DEADLINE_MS=$((WINDOW * 1000)) timeout -s KILL "$WINDOW" /enet-lab3 eth0 $MYET $PEERS
echo "=== ENET-LAB3-DONE ==="
/bin/busybox poweroff -f
INIT
    chmod +x "$stage/init"
    ( cd "$stage" && find . | cpio -o -H newc 2>/dev/null | gzip ) > "$WORK/n$myet.gz"
    echo "$WORK/n$myet.gz"
}

boot() {   # $1=initrd  $2=nic-arg  $3=logfile
    timeout -k 5 "$TMO" "$QEMU" -M imx93-11x11-evk -audio driver=none -smp 3 -m "$MEM" \
        -display none -kernel "$IMAGE" -dtb "$DTB" -initrd "$1" \
        -append "console=ttyLP0,115200 cpuidle.off=1 rdinit=/init" \
        -nic "$2" -nic user \
        -serial file:"$3" -serial null >/dev/null 2>&1 &
}

report() {  # $1=logfile  $2=label
    echo "================== $2 =================="
    grep -aE 'ENET-LAB3' "$1" | grep -avE '^\[ *[0-9]+\.[0-9]+\]' | sed 's#.*/##'
}

case "$MODE" in
# --------------------------------------------------------------------------- #
selftest)
    PORT=${PORT:-14893}
    A_IRD=$(build_initrd 0x88B9 0x88BA)
    B_IRD=$(build_initrd 0x88BA 0x88B9)
    ALOG="$WORK/a.log"; BLOG="$WORK/b.log"
    echo "== booting 93 node A (0x88B9, socket listen :$PORT) =="
    boot "$A_IRD" "socket,listen=127.0.0.1:$PORT" "$ALOG"; SPID=$!; reap_track $SPID
    sleep 2
    echo "== booting 93 node B (0x88BA, socket connect :$PORT) =="
    boot "$B_IRD" "socket,connect=127.0.0.1:$PORT" "$BLOG"; CPID=$!; reap_track $CPID
    wait $SPID $CPID 2>/dev/null
    report "$ALOG" "NODE A 0x88B9"
    report "$BLOG" "NODE B 0x88BA"
    apass=$(grep -ac 'ENET-LAB3 PASS' "$ALOG"); bpass=$(grep -ac 'ENET-LAB3 PASS' "$BLOG")
    acorr=$(grep -ac 'ENET-LAB3 CORRUPT' "$ALOG"); bcorr=$(grep -ac 'ENET-LAB3 CORRUPT' "$BLOG")
    # each must have accepted the OTHER's body (peer 0x88Bx body OK)
    asaw=$(grep -ac 'rx: peer 0x88ba body OK' "$ALOG")
    bsaw=$(grep -ac 'rx: peer 0x88b9 body OK' "$BLOG")
    echo "------------------------------------------------------"
    echo "A: PASS=$apass CORRUPT=$acorr saw-B-body=$asaw   B: PASS=$bpass CORRUPT=$bcorr saw-A-body=$bsaw"
    [ "$apass" -ge 1 ] && [ "$bpass" -ge 1 ] || die "both nodes must reach PASS"
    [ "$acorr" -eq 0 ] && [ "$bcorr" -eq 0 ] || die "a well-formed segment must have 0 CORRUPT"
    [ "$asaw" -ge 1 ] && [ "$bsaw" -ge 1 ] || die "each node must token-verify the other's body"
    echo "PASS: two i.MX 93 nodes token-verified each other over FEC (magic+self-ethertype+fill+fresh-incarnation), 0 corrupt"
    ;;
# --------------------------------------------------------------------------- #
wireshape)
    # Guest sends beacons to udp=UD (host binds UD to capture); the exact udp
    # wiring rt1180's switch ports use. Boot twice; assert per-boot incarnation.
    UD=${UD:-45932}; LC=${LC:-45931}
    HOSTVAL="$WORK/validate.py"
    cat > "$HOSTVAL" <<'PY'
import socket, struct, sys
UD = int(sys.argv[1]); FRAME=64; MAGIC=0xB5B6B7C0; ET=0x88B9; LEGACY=0x5A5A5A5A
s=socket.socket(socket.AF_INET,socket.SOCK_DGRAM); s.setsockopt(socket.SOL_SOCKET,socket.SO_REUSEADDR,1)
s.bind(("127.0.0.1",UD)); s.settimeout(float(sys.argv[2]))
good=0; incarn=None
try:
    while True:
        d,_=s.recvfrom(2048)
        if len(d)!=FRAME: continue
        et=(d[12]<<8)|d[13]
        if et!=ET: continue
        if struct.unpack(">I",d[14:18])[0]!=MAGIC: continue
        if ((d[18]<<8)|d[19])!=ET: continue                 # self-ethertype agrees
        inc=struct.unpack(">I",d[24:28])[0]
        if inc==0 or inc==LEGACY: continue                  # must be a real per-boot nonce
        if any(b!=0x5A for b in d[28:64]): continue          # fill intact
        good+=1; incarn=inc
        if good>=5: break
except socket.timeout:
    pass
print("VALIDATED %d well-formed 0x88B9 beacons; incarnation=%s" %
      (good, ("0x%08x"%incarn) if incarn is not None else "NONE"))
sys.exit(0 if good>=5 else 1)
PY
    run_once() {   # -> prints "incarnation=0x...."
        local log=$1
        python3 "$HOSTVAL" "$UD" "$((WINDOW+15))" > "$WORK/val.out" 2>&1 &
        local vpid=$!
        # A dummy required peer (0x88BF, unused block slot) so the tool starts and
        # beacons; it will never PASS (peer absent) but we only validate emitted frames.
        IRD=$(build_initrd 0x88B9 0x88BF)
        boot "$IRD" "socket,udp=127.0.0.1:$UD,localaddr=127.0.0.1:$LC" "$log"; SPID=$!; reap_track $SPID
        wait $vpid 2>/dev/null; vrc=$?
        wait $SPID 2>/dev/null
        cat "$WORK/val.out"
        return $vrc
    }
    echo "== boot 1: host-validate emitter shape over udp:$UD =="
    run_once "$WORK/w1.log" || die "boot 1: emitter frame shape invalid"
    INC1=$(grep -o 'incarnation=0x[0-9a-f]*' "$WORK/val.out" | head -1)
    echo "== boot 2: prove the incarnation nonce is PER-BOOT =="
    run_once "$WORK/w2.log" || die "boot 2: emitter frame shape invalid"
    INC2=$(grep -o 'incarnation=0x[0-9a-f]*' "$WORK/val.out" | head -1)
    echo "------------------------------------------------------"
    echo "boot1 $INC1 ; boot2 $INC2"
    [ -n "$INC1" ] && [ "$INC1" != "$INC2" ] || die "incarnation did not change across boots (nonce is a constant)"
    echo "PASS: 0x88B9 emits a spec-valid v2 body over FEC, and its incarnation is provably per-boot"
    ;;
# --------------------------------------------------------------------------- #
attach)
    NIC=${2:-}; MYET=${3:-0x88B9}; shift 3 2>/dev/null || shift $#
    PEERS="$*"
    [ -n "$NIC" ] || die "usage: run.sh attach '<nic-arg>' <my_et> <peer_et>..."
    echo "== attaching 93 node $MYET to: $NIC  (watching: ${PEERS:-none required, observe-only}) =="
    IRD=$(build_initrd "$MYET" $PEERS)
    LOG="$WORK/attach.log"
    boot "$IRD" "$NIC" "$LOG"; SPID=$!; reap_track $SPID
    wait $SPID 2>/dev/null
    report "$LOG" "93 NODE $MYET on the shared segment"
    pass=$(grep -ac 'ENET-LAB3 PASS' "$LOG")
    echo "------------------------------------------------------"
    if [ "$pass" -ge 1 ]; then
        echo "PASS: 0x88B9 token-verified its required peer(s) on the shared segment ($pass beat(s))"
    else
        echo "NO-PASS: $MYET did not close its required-peer gate - see the log above for what it saw (CORRUPT/LEGACY/observed-only) vs what was missing"
        exit 1
    fi
    ;;
*)
    die "unknown mode '$MODE' (selftest | wireshape | attach)"
    ;;
esac
