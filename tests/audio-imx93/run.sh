#!/usr/bin/env bash
#
# Audio playback check for the i.MX93 QEMU machine.
#
# The NXP BSP builds the ASoC stack as modules, so this chains a tiny overlay
# initramfs (/myinit) onto the imx-image-core rootfs, modprobes the SAI/codec
# drivers so the cards register, and plays a generated square wave on the
# wm8962/SAI3 card via the pcm_play oracle. Expect:
#
#   0 [btscoaudio  ]: simple-card  - bt-sco-audio
#   1 [wm8962audio ]: fsl-asoc-card - wm8962-audio   (SAI3 -> wm8962 codec)
#   PLAY[hw:1,0]: PASS (48000 frames)
#
# A clean PASS with "drain -> Success" means the eDMA3 cyclic datapath paced
# the whole stream into the SAI3 TX FIFO at the audio rate, with no under-run.
# Set WAV=/path/out.wav to also capture the played samples (the square wave)
# through QEMU's audio backend to a real wav file.
#
# There is no aplay in the BSP, so pcm_play.c is cross-compiled here. It needs
# ALSA headers and the on-target libasound.so.2 (extracted from the rootfs):
#   ALSA_SYSROOT=<dir with usr/include/alsa>   (a BSP recipe-sysroot works)
#   CROSS=aarch64-linux-gnu-
#
# Override any path via env:  KERNEL=/path/Image DTB=/path.dtb BASE_INITRD=...
set -u

HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
DEPLOY=${DEPLOY:-$HOME/Documents/nxp/linux/imx-yocto-bsp/build-imx93/tmp/deploy/images/imx93evk}

QEMU=${QEMU:-$REPO/build-imx93/qemu-system-aarch64}
KERNEL=${KERNEL:-$DEPLOY/Image}
DTB=${DTB:-$DEPLOY/imx93-11x11-evk.dtb}
# imx-image-core rootfs cpio: needs /lib/modules (the ASoC drivers are =m).
BASE_INITRD=${BASE_INITRD:-$HOME/Documents/nxp/imx93-initramfs.cpio.gz}
CROSS=${CROSS:-aarch64-linux-gnu-}
ALSA_SYSROOT=${ALSA_SYSROOT:-}
WAV=${WAV:-}

need() { [ -e "$2" ] || { echo "error: $3 not found: $2" >&2; exit 1; }; }
need QEMU        "$QEMU"        "qemu-system-aarch64 (build it first)"
need KERNEL      "$KERNEL"      "kernel Image"
need DTB         "$DTB"         "device tree"
need BASE_INITRD "$BASE_INITRD" "base imx-image-core initramfs cpio.gz"

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
install -m755 "$HERE/myinit" "$TMP/myinit"

# Build the playback oracle if an ALSA sysroot is available.
PLAY_FILES="myinit"
if [ -n "$ALSA_SYSROOT" ] && command -v "${CROSS}gcc" >/dev/null; then
    # Link against the actual on-target libasound.so.2 from the rootfs.
    ( cd "$TMP" && zcat "$BASE_INITRD" | cpio -id 'usr/lib/libasound.so.2*' \
        2>/dev/null )
    LASOUND=$(ls "$TMP"/usr/lib/libasound.so.2.* 2>/dev/null | head -1)
    if [ -n "$LASOUND" ] && "${CROSS}gcc" -O2 -Wall \
            -I"$ALSA_SYSROOT/usr/include" -o "$TMP/pcm_play" "$HERE/pcm_play.c" \
            "$LASOUND" -Wl,--allow-shlib-undefined 2>/dev/null; then
        PLAY_FILES="myinit
pcm_play"
        echo "built pcm_play oracle"
    else
        echo "note: could not build pcm_play; card-registration check only" >&2
    fi
else
    echo "note: set ALSA_SYSROOT to build pcm_play; card-registration only" >&2
fi

( cd "$TMP" && printf '%s\n' "$PLAY_FILES" | cpio -o -H newc 2>/dev/null \
    > overlay.cpio )
cat "$BASE_INITRD" "$TMP/overlay.cpio" > "$TMP/combined.cpio.gz"

AUDIO=(-audio driver=none)  # muted by default; WAV= opts into capture
[ -n "$WAV" ] && AUDIO=(-audio "driver=wav,path=$WAV")

set -x
exec "$QEMU" -M imx93-11x11-evk -m 4G -display none "${AUDIO[@]}" \
    -kernel "$KERNEL" -dtb "$DTB" -initrd "$TMP/combined.cpio.gz" \
    -append "console=ttyLP0,115200 cpuidle.off=1 rdinit=/myinit ignore_loglevel" \
    -serial mon:stdio -serial null
