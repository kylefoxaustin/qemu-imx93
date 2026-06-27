#!/usr/bin/env bash
#
# i.MX93 media-conformance sweep: point real upstream conformance tools at the
# live media datapaths and let them flush model bugs a smoke test can't.
#   display lane -> modetest (libdrm) over LCDIFv3 -> DSI   (rm67199 DTB)
#   camera  lane -> v4l2-compliance (v4l-utils) over ISI     (mt9m114 DTB)
# Each lane: cross-built tool + a guest battery -> overlay initramfs -> one boot
# -> MEDIA: markers on the console -> host scoreboard. See README.md.
#
# The conformance binaries are cross-built artifacts (not in the repo). Point
# MEDIA_BIN_DIR at a dir holding `modetest` and `v4l2-compliance` (aarch64).
# Other paths overridable via env (KERNEL/DTB/QEMU/BASE_INITRD/DURATION).
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
DEPLOY=${DEPLOY:-$HOME/Documents/nxp/linux/imx-yocto-bsp/build-imx93/tmp/deploy/images/imx93evk}
export QEMU=${QEMU:-$REPO/build-imx93/qemu-system-aarch64}
export KERNEL=${KERNEL:-$DEPLOY/Image}
export BASE_INITRD=${BASE_INITRD:-$HOME/Documents/nxp/imx93-initramfs.cpio.gz}
export DURATION=${DURATION:-240}
export OUTDIR=${OUTDIR:-/tmp/media-conformance-out}
MEDIA_BIN_DIR=${MEDIA_BIN_DIR:-$HERE/bin}
CROSS=${CROSS:-aarch64-linux-gnu-}
LANES=${1:-all}

need() { [ -e "$2" ] || { echo "error: $3 not found: $2" >&2; exit 1; }; }
need QEMU "$QEMU" "qemu"; need KERNEL "$KERNEL" "kernel Image"
need BASE "$BASE_INITRD" "base initramfs"
mkdir -p "$OUTDIR"

run_lane() {
    local lane=$1 dtb=$2 battery=$3; shift 3
    need DTB "$dtb" "$lane DTB"
    local ovl="$OUTDIR/$lane-overlay"; rm -rf "$ovl"; mkdir -p "$ovl"
    cp "$HERE/$battery" "$ovl/battery.sh"
    for bin in "$@"; do need BIN "$MEDIA_BIN_DIR/$bin" "$lane tool '$bin'"; cp "$MEDIA_BIN_DIR/$bin" "$ovl/$bin"; done
    echo "=== LANE: $lane (dtb=$(basename "$dtb"), tools: $*) ==="
    DTB="$dtb" LANE="$lane" OVERLAY_DIR="$ovl" python3 "$HERE/media_conformance.py"
}

[ "$LANES" = "display" ] || [ "$LANES" = "all" ] && \
    run_lane display "$DEPLOY/imx93-11x11-evk-rm67199.dtb" display_battery.sh modetest
[ "$LANES" = "camera" ] || [ "$LANES" = "all" ] && \
    run_lane camera "$DEPLOY/imx93-11x11-evk-mt9m114.dtb" camera_battery.sh v4l2-compliance v4l2_cap

echo
echo "=== SCOREBOARD ==="
bash "$HERE/score.sh" "$OUTDIR"
