#!/usr/bin/env bash
#
# Cortex-M33 bring-up test for the i.MX93 QEMU machine.
#
# Loads the minimal M33 blob (build.sh -> m33_fw.bin) into the A55-view ITCM
# alias, boots the A55 Linux as usual, and checks via the QEMU monitor that the
# M33 actually ran: its firmware writes a magic word to DTCM[0] and bumps a
# heartbeat counter in DTCM[1], visible to the A55 system view at 0x20200000.
#
# PASS = DTCM[0] == 0xc0ffee33 and the counter advances between two reads.
# Run ./build.sh first (needs arm-none-eabi-gcc) to produce m33_fw.bin.
#
# Override paths via env: KERNEL=/path/Image DTB=/path.dtb BASE_INITRD=...
set -u

HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
DEPLOY=${DEPLOY:-$HOME/Documents/nxp/linux/imx-yocto-bsp/build-imx93/tmp/deploy/images/imx93evk}

QEMU=${QEMU:-$REPO/build-imx93/qemu-system-aarch64}
KERNEL=${KERNEL:-$DEPLOY/Image}
DTB=${DTB:-$DEPLOY/imx93-11x11-evk.dtb}
BASE_INITRD=${BASE_INITRD:-$HOME/Documents/nxp/imx93-initramfs.cpio.gz}
FW=${FW:-$HERE/m33_fw.bin}

need() { [ -e "$2" ] || { echo "error: $3 not found: $2" >&2; exit 1; }; }
need QEMU        "$QEMU"        "qemu-system-aarch64 (build it first)"
need KERNEL      "$KERNEL"      "kernel Image"
need DTB         "$DTB"         "device tree"
need BASE_INITRD "$BASE_INITRD" "base imx-image-core initramfs cpio.gz"
need FW          "$FW"          "M33 firmware (run ./build.sh)"

MON=$(mktemp -u /tmp/m33mon.XXXXXX.sock)
LOG=$(mktemp /tmp/m33boot.XXXXXX.log)
cleanup() { [ -n "${QPID:-}" ] && kill "$QPID" 2>/dev/null; rm -f "$MON" "$LOG"; }
trap cleanup EXIT

# Load at the A55 view of ITCM + 0x20000 (0x201E0000) - the M33 reset VTOR,
# matching where NXP M33 firmware links its vector table.
setsid "$QEMU" -M imx93-11x11-evk -audio driver=none -m 4G -display none \
    -kernel "$KERNEL" -dtb "$DTB" -initrd "$BASE_INITRD" \
    -append "console=ttyLP0,115200 cpuidle.off=1 rdinit=/sbin/init ignore_loglevel" \
    -device loader,file="$FW",addr=0x201E0000 \
    -monitor unix:"$MON",server,nowait \
    -serial file:"$LOG" -serial null >/dev/null 2>&1 < /dev/null &
QPID=$!
sleep 10

python3 - "$MON" <<'PY'
import socket, sys, time, re
mon = sys.argv[1]
def rd(s):
    s.sendall(b"xp /3xw 0x20200000\n"); time.sleep(0.5)
    return s.recv(4096).decode(errors="replace")
def words(t):
    m = re.search(r"20200000:\s+(0x[0-9a-f]+)\s+(0x[0-9a-f]+)", t)
    return (int(m.group(1), 16), int(m.group(2), 16)) if m else (None, None)
s = socket.socket(socket.AF_UNIX); s.connect(mon); time.sleep(0.3); s.recv(4096)
m0, c0 = words(rd(s)); time.sleep(1.0); m1, c1 = words(rd(s)); s.close()
print(f"DTCM[0]=0x{m0:08x} counter {c0:#x} -> {c1:#x}" if m0 is not None
      else "could not read DTCM")
ok = (m0 == 0xc0ffee33 and m1 == 0xc0ffee33 and c1 != c0)
print("PASS: M33 is executing" if ok else "FAIL: M33 did not run")
sys.exit(0 if ok else 1)
PY