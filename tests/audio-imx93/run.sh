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

# Build the playback oracle. The ALSA API headers are architecture-independent,
# so the host's (libasound2-dev) serve as the sysroot; only the library must be
# the on-target one, taken from the rootfs.
( cd "$TMP" && zcat "$BASE_INITRD" | cpio -id 'usr/lib/libasound.so.2*' 2>/dev/null )
LASOUND=$(ls "$TMP"/usr/lib/libasound.so.2.* 2>/dev/null | head -1)
if ! command -v "${CROSS}gcc" >/dev/null || [ -z "$LASOUND" ] || \
   ! "${CROSS}gcc" -O2 -Wall -I"$ALSA_SYSROOT/usr/include" \
        -o "$TMP/pcm_play" "$HERE/pcm_play.c" "$LASOUND" \
        -Wl,--allow-shlib-undefined 2>/dev/null; then
    # Refuse to degrade into a weaker check that still looks like success. This
    # test's whole claim is "real PCM reached the SAI"; without the oracle it
    # cannot render that verdict, so it must not render one at all.
    echo "SKIP: cannot build the pcm_play oracle (need ${CROSS}gcc, the ALSA"
    echo "      headers under ALSA_SYSROOT, and libasound.so.2 in the rootfs)."
    echo "      Without it there is no audio verdict, so this test claims none."
    exit 0
fi

#
# The capture is NOT optional, and that is the point.
#
# driver=wav opens a FILE, never a host device - so it is simultaneously the
# MUTE (this test can never reach the developer's speakers, whatever the host
# audio setup) and the EVIDENCE (the samples ARE the assertion). Making it
# mandatory means the safe path is no longer the one you have to remember: the
# test cannot render a verdict without it.
#
# RATES selects the playback rate(s). The rate is an argument rather than a
# constant because a model that assumes one rate is invisible to a test that
# only ever asks for that rate - see the KNOWN GAP below.
#
#   RATES="48000 16000" bash run.sh
#
# demonstrates it: on this board the SAI is a bit-clock SLAVE (TCR2.BCD_MSTR is
# clear - the wm8962 codec drives BCLK/LRCLK), so the frame rate is set in the
# CODEC over I2C and is not derivable from any SAI register. Our SAI opens its
# audio backend at a hardcoded 48 kHz, so a 16 kHz stream is clocked out three
# times too fast: the capture comes back a third of a second long with the tone
# transposed from 145 Hz up to 436 Hz. Closing it needs the wm8962 model - today
# a register store that never decodes its clocking - to derive its rate and hand
# it to the SAI. Declared, demonstrable, and not silent.
#
RATES=${RATES:-48000}
LOG=$TMP/console.log
WAVDIR=${WAVDIR:-$TMP}

fail() { echo "FAIL: $*"; echo "--- console tail ---"; tail -20 "$LOG"; exit 1; }

for RATE in $RATES; do
    echo "$RATE" > "$TMP/rate"
    ( cd "$TMP" && printf 'myinit\npcm_play\nrate\n' | cpio -o -H newc \
        2>/dev/null > overlay.cpio )
    cat "$BASE_INITRD" "$TMP/overlay.cpio" > "$TMP/combined.cpio.gz"

    WAV="$WAVDIR/capture-$RATE.wav"
    rm -f "$WAV"

    timeout -s KILL "${TMO:-240}" "$QEMU" -M imx93-11x11-evk -m 4G -display none \
        -audio "driver=wav,path=$WAV" \
        -kernel "$KERNEL" -dtb "$DTB" -initrd "$TMP/combined.cpio.gz" \
        -append "console=ttyLP0,115200 cpuidle.off=1 rdinit=/myinit ignore_loglevel" \
        -serial "file:$LOG" -serial null >/dev/null 2>&1 || true

    grep -q 'wm8962audio' "$LOG" || fail "[$RATE Hz] the wm8962/SAI3 card never registered"
    # Match any card index: the kernel assigns them in registration order, so
    # they move. What must hold is that the wm8962 card played and drained, not
    # that it happened to land at a particular number.
    grep -qE 'PLAY\[hw:[0-9]+,0\]: PASS' "$LOG" || \
        fail "[$RATE Hz] pcm_play did not report PASS (the eDMA3 -> SAI3 datapath did not drain the stream)"
    [ -s "$WAV" ] || fail "[$RATE Hz] no samples were captured: $WAV is missing or empty"

    # The samples are the assertion: peak (nothing scaled them), duration
    # (nothing truncated them) and TONE (they were clocked out at the rate the
    # guest asked for).
    python3 "$HERE/check_wav.py" "$WAV" "$RATE" || \
        fail "[$RATE Hz] the captured samples are not the square wave that was played"
done

echo "PASS: real PCM through wm8962/SAI3 -> eDMA3 cyclic, verified at $RATES Hz"
