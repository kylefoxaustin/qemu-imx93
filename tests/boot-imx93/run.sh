#!/usr/bin/env bash
#
# Boot real NXP i.MX 93 Linux on the QEMU imx93-11x11-evk machine.
#
# Unlike the i.MX 95, the i.MX 93 has NO System Manager: there is no M33 SM
# firmware to load and no SCMI server to stand up. Linux programs the CCM /
# ANATOP / etc. directly, so this is a plain kernel + DTB (+ optional rootfs)
# boot.
#
# Artifacts come from env vars; the defaults point at the imx93evk Yocto
# deploy directory this project builds (see tests/../bsp-build-plan). Override
# any of them:
#
#   QEMU=./build-imx93/qemu-system-aarch64 \
#   KERNEL=/path/to/Image \
#   DTB=/path/to/imx93-11x11-evk.dtb \
#   INITRD=/path/to/rootfs.cpio.gz \
#       tests/boot-imx93/run.sh
#
# INITRD is OPTIONAL. With no rootfs the kernel still boots all the way
# through driver probe and only panics at "Unable to mount root fs" - which
# is exactly the triage data we want for first bring-up (does the console
# come up? do CCM/ANATOP/GIC/timer probe? where does it die?).
#
# icount is OFF by default (debug-determinism tool, not a normal-boot config).
# Enable when chasing a timer/IRQ race:  ICOUNT=1 tests/boot-imx93/run.sh
#
# Extra QEMU args (e.g. -d unimp, -monitor stdio) pass through after the name.
set -u

HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
DEPLOY=${DEPLOY:-$HOME/Documents/nxp/linux/imx-yocto-bsp/build-imx93/tmp/deploy/images/imx93evk}

QEMU=${QEMU:-$REPO/build-imx93/qemu-system-aarch64}
KERNEL=${KERNEL:-$DEPLOY/Image}
DTB=${DTB:-$DEPLOY/imx93-11x11-evk.dtb}
INITRD=${INITRD:-}

# earlycon on LPUART1 (console = ttyLP0). The lpuart32 earlycon address is the
# register base + 0x10 (the driver's reg_off on i.MX). cpuidle.off=1 is a
# conservative first-boot default (QEMU can't wake a CPU whose GIC cpuif was
# disabled in idle - the same core limitation hit on i.MX 95).
CMDLINE=${CMDLINE:-"earlycon=lpuart32,mmio32,0x44380010 console=ttyLP0,115200 cpuidle.off=1"}

need() {  # need VAR "path" "description"
    [ -e "$2" ] && return 0
    echo "error: $3 not found:" >&2
    echo "    $2" >&2
    echo "  set \$$1 to its location, e.g. $1=/path/... tests/boot-imx93/run.sh" >&2
    exit 1
}
need QEMU   "$QEMU"   "qemu-system-aarch64 (build it first)"
need KERNEL "$KERNEL" "kernel Image"
need DTB    "$DTB"    "device tree imx93-11x11-evk.dtb"

ICOUNT_ARGS=()
[ -n "${ICOUNT:-}" ] && ICOUNT_ARGS=(-icount shift=auto)

INITRD_ARGS=()
if [ -n "$INITRD" ]; then
    need INITRD "$INITRD" "initramfs / rootfs cpio"
    INITRD_ARGS=(-initrd "$INITRD")
    CMDLINE="$CMDLINE rdinit=/init"
fi

set -x
exec "$QEMU" -M imx93-11x11-evk -audio driver=none -m 2G -display none \
    "${ICOUNT_ARGS[@]}" \
    -kernel "$KERNEL" -dtb "$DTB" "${INITRD_ARGS[@]}" \
    -append "$CMDLINE" \
    -serial mon:stdio -serial null \
    "$@"
