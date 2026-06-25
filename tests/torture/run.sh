#!/usr/bin/env bash
#
# i.MX93 max-concurrency torture shakeout.
#
# Boots the core-image-weston desktop and hammers every datapath at once -
# Wayland desktop + an animating client, Ethos-U65 mobilenet inference, the full
# audio surface (wm8962 play+capture, SPDIF, MICFIL), CPU stress on both A55s,
# SD storage churn and network - then checks that none of them wedged and the
# display kept scanning out frames. See README.md for the approach.
#
# Needs external (non-repo) assets, overridable via env:
#   WIC        core-image-weston .wic (use-g2d=false, resized to >=1G); the
#              desktop vehicle.   ~/Documents/nxp/imx93-weston.wic
#   EIQ_DIR    eIQ delegate stack: usr/lib/libethosu*.so + libtensorflow-lite,
#              opt/benchmark_model, opt/mobilenet_vela.tflite.
#   ETHOSU_FW  the Ethos-U M33 firmware (staged into the guest /lib/firmware).
#   ALSA_INC   ALSA headers (for cross-building the pcm tools).
#   ALSA_LIB   on-target libasound.so.2.* (linked + shipped in the overlay).
# Plus the usual KERNEL / DTB / QEMU / DURATION.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
HOMEK=${HOME}
DEPLOY=${DEPLOY:-$HOMEK/Documents/nxp/linux/imx-yocto-bsp/build-imx93/tmp/deploy/images/imx93evk}
export QEMU=${QEMU:-$REPO/build-imx93/qemu-system-aarch64}
export KERNEL=${KERNEL:-$DEPLOY/Image}
export DTB=${DTB:-$DEPLOY/imx93-11x11-evk.dtb}
export WIC=${WIC:-$HOMEK/Documents/nxp/imx93-weston.wic}
export DURATION=${DURATION:-600}
EIQ_DIR=${EIQ_DIR:-$HOMEK/imx93-npu-testkit/eiq-overlay}
ETHOSU_FW=${ETHOSU_FW:-$HOMEK/Documents/nxp/imx93-rootfs/lib/firmware/ethosu_firmware}
ALSA_INC=${ALSA_INC:-}
ALSA_LIB=${ALSA_LIB:-$HOMEK/Documents/nxp/imx93-rootfs/usr/lib/libasound.so.2.0.0}
CROSS=${CROSS:-aarch64-linux-gnu-}
export OVERLAY=${OVERLAY:-/tmp/torture-overlay}
export OUTDIR=${OUTDIR:-/tmp/torture-out}

need() { [ -e "$2" ] || { echo "error: $3 not found: $2" >&2; exit 1; }; }
need QEMU "$QEMU" "qemu (build it)"; need KERNEL "$KERNEL" "kernel Image"
need DTB "$DTB" "device tree"; need WIC "$WIC" "core-image-weston .wic"
need EIQ "$EIQ_DIR/opt/benchmark_model" "eIQ benchmark_model"
need EIQ "$EIQ_DIR/opt/mobilenet_vela.tflite" "vela mobilenet"
need FW "$ETHOSU_FW" "Ethos-U firmware"

# Assemble the 9p overlay.
rm -rf "$OVERLAY"; mkdir -p "$OVERLAY"/{usr/lib,opt,firmware,progress}
cp -a "$EIQ_DIR"/usr/lib/* "$OVERLAY/usr/lib/"
cp -a "$EIQ_DIR"/opt/benchmark_model "$EIQ_DIR"/opt/mobilenet_vela.tflite "$OVERLAY/opt/"
cp -a "$ETHOSU_FW" "$OVERLAY/firmware/ethosu_firmware"
cp -a "$HERE/launcher.sh" "$OVERLAY/launcher.sh"

# Cross-build the ALSA pcm tools for the audio torture (optional - skipped if no
# headers; the launcher then just won't drive audio).
if [ -n "$ALSA_INC" ] && [ -f "$ALSA_LIB" ] && command -v "${CROSS}gcc" >/dev/null; then
    for t in pcm_play pcm_capture; do
        "${CROSS}gcc" -O2 -Wall -I"$ALSA_INC" -o "$OVERLAY/opt/$t" \
            "$REPO/tests/audio-imx93/$t.c" "$ALSA_LIB" \
            -Wl,--allow-shlib-undefined 2>/dev/null \
            && echo "built $t" || echo "note: $t build failed (audio skipped)"
    done
    cp -a "$ALSA_LIB" "$OVERLAY/usr/lib/libasound.so.2.0.0"
    ln -sf libasound.so.2.0.0 "$OVERLAY/usr/lib/libasound.so.2"
else
    echo "note: ALSA_INC unset or libasound missing - audio streams skipped" >&2
fi

echo "overlay at $OVERLAY ($(du -sh "$OVERLAY" | cut -f1)); booting torture (${DURATION}s soak)..."
exec python3 "$HERE/torture.py"
