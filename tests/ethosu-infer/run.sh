#!/usr/bin/env bash
#
# Ethos-U65 end-to-end inference test for the i.MX93 QEMU machine.
#
# Stages a Vela-compiled int8 model + a sample IFM + the ethosu_infer guest app
# into the rootfs, boots the on-demand M33 path, and runs a full inference:
#   open /dev/ethosu0 -> M33 boots -> NETWORK/INFERENCE/INVOKE -> NPU kick ->
#   the in-QEMU Ethos-U executor (hw/npu/) runs the command stream and produces
#   the OFM + IRQ -> the guest reads the classification out of its OFM buffer.
#
# The executor's OFM is bit-exact with the host TFLite reference; compare with
#   tests/ethosu-infer/host/ethosu_host_infer.py model_int8.tflite sample.bin out
#
# Set ETHOSU_TRACE=1 to dump the firmware's NPU register traffic to qemu stderr.
#
# Env overrides: QEMU=, KERNEL=, DTB=, BASE_INITRD=, MODEL=, IFM=, LOG=
set -u

HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
DEPLOY=${DEPLOY:-$HOME/Documents/nxp/linux/imx-yocto-bsp/build-imx93/tmp/deploy/images/imx93evk}

QEMU=${QEMU:-$REPO/build-imx93/qemu-system-aarch64}
KERNEL=${KERNEL:-$DEPLOY/Image}
DTB=${DTB:-$DEPLOY/imx93-11x11-evk.dtb}
BASE_INITRD=${BASE_INITRD:-$HOME/Documents/nxp/imx93-initramfs.cpio.gz}
MODEL=${MODEL:-$HERE/host/model_int8_vela.tflite}
IFM=${IFM:-$HERE/host/sample_top.bin}
APP=${APP:-$HERE/ethosu_infer}
LOG=${LOG:-/tmp/ethosu-infer.log}
export ETHOSU_TRACE=${ETHOSU_TRACE:-0}

need() { [ -e "$2" ] || { echo "error: $3 not found: $2" >&2; exit 1; }; }
need QEMU "$QEMU" "qemu"; need KERNEL "$KERNEL" "kernel"; need DTB "$DTB" "dtb"
need BASE_INITRD "$BASE_INITRD" "initrd"; need MODEL "$MODEL" "vela model"
need IFM "$IFM" "ifm sample"

if [ ! -x "$APP" ]; then
    echo "building ethosu_infer..."
    aarch64-linux-gnu-gcc -static -O2 -Wall -o "$APP" "$HERE/ethosu_infer.c" || exit 1
fi

TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
# NB: do NOT create /lib here - in this rootfs /lib is a symlink to /usr/lib,
# and overlaying a real /lib dir shadows it, hiding /lib/firmware/ethosu_firmware
# (the M33 firmware the kernel needs to boot the core). Stage the model at root.
cp "$APP" "$TMP/ethosu_infer"
cp "$MODEL" "$TMP/model_vela.tflite"
cp "$IFM" "$TMP/sample.bin"
cat > "$TMP/myinit" <<'EOF'
#!/bin/sh
PATH=/sbin:/usr/sbin:/bin:/usr/bin; export PATH
mount -t proc proc /proc; mount -t sysfs sysfs /sys; mount -t devtmpfs devtmpfs /dev
sleep 3
echo "=== ETHOSU INFER TEST ==="
/ethosu_infer /model_vela.tflite /sample.bin
echo "--- dmesg (ethosu) ---"
dmesg | grep -iE "ethosu|remoteproc|Oops" | tail -15
echo "=== ETHOSU INFER TEST DONE ==="
while true; do sleep 5; done
EOF
chmod +x "$TMP/myinit"
( cd "$TMP" && find ./myinit ./ethosu_infer ./sample.bin ./model_vela.tflite \
   | cpio -o -H newc 2>/dev/null > o.cpio )
cat "$BASE_INITRD" "$TMP/o.cpio" > "$TMP/c.cpio.gz"

M33CON=${M33CON:-/tmp/ethosu-infer-m33.log}
echo "M33 console -> $M33CON ; full log -> $LOG"
set -x
"$QEMU" -M imx93-11x11-evk -audio driver=none -m 4G -display none \
    -kernel "$KERNEL" -dtb "$DTB" -initrd "$TMP/c.cpio.gz" \
    -append "console=ttyLP0,115200 cpuidle.off=1 rdinit=/myinit ignore_loglevel" \
    -serial mon:stdio -serial "file:$M33CON" 2>&1 | tee "$LOG"
