#!/bin/sh
# i.MX93 migration (vmstate) round-trip smoke.
#
# Every device model carries a VMStateDescription, but until this test nothing
# ever exercised save+load - a version bump or a mismatched field would only
# surface the first time someone snapshotted a guest. This boots the machine to
# userspace, MIGRATES it to a file (serialising RAM + every device's vmstate),
# loads that stream into a second QEMU via -incoming, and resumes it. PASS means
# every device's vmstate round-tripped and the restored guest runs.
#
# Asset-gated like tests/audio-imx93: override paths via env. SKIPs (does not
# fail) when the kernel/dtb/initramfs or python3 are unavailable.
set -u

HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
DEPLOY=${DEPLOY:-$HOME/Documents/nxp/linux/imx-yocto-bsp/build-imx93/tmp/deploy/images/imx93evk}
QEMU=${QEMU:-$REPO/build-imx93/qemu-system-aarch64}
KERNEL=${KERNEL:-$DEPLOY/Image}
DTB=${DTB:-$DEPLOY/imx93-11x11-evk.dtb}
BASE_INITRD=${BASE_INITRD:-$HOME/Documents/nxp/imx93-initramfs.cpio.gz}

have() { [ -e "$2" ] || { echo "SKIP: $3 not found: $2"; exit 0; }; }
have QEMU        "$QEMU"        "qemu-system-aarch64"
have KERNEL      "$KERNEL"      "kernel Image"
have DTB         "$DTB"         "device tree"
have BASE_INITRD "$BASE_INITRD" "base initramfs"
command -v python3 >/dev/null || { echo "SKIP: python3 not found"; exit 0; }
command -v cpio    >/dev/null || { echo "SKIP: cpio not found"; exit 0; }

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

# A tiny init that mounts and then spins, so the guest stays alive for the
# snapshot (the stock imx-image-core initramfs powers off with no getty on the
# serial console). By the time init runs, clk-imx93 has set the CCM roots/LPCGs
# and the TPMs are probed, so device vmstate is populated - not all-zero.
install -m755 "$HERE/myinit" "$TMP/myinit"
( cd "$TMP" && printf 'myinit\n' | cpio -o -H newc 2>/dev/null > overlay.cpio )
cat "$BASE_INITRD" "$TMP/overlay.cpio" > "$TMP/combined.cpio.gz"

python3 "$HERE/migrate_roundtrip.py" \
    "$QEMU" "$KERNEL" "$DTB" "$TMP/combined.cpio.gz" "$TMP"
rc=$?

if [ "$rc" -eq 0 ]; then
    echo "PASS: vmstate round-trip (migrate -> file -> -incoming -> resume) clean"
else
    echo "FAIL: vmstate round-trip failed (rc=$rc)"
fi
exit "$rc"
