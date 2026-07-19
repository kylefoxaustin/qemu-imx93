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

# The capture oracle for the MICFIL (PDM) card - same toolchain/sysroot as the
# playback one. Its job is to drive the MICFIL RX FIFO -> eDMA1 datapath AND to
# make the model emit its capture-start trace, so run.sh can check the derived
# sample rate tracks the requested one. If it will not build we drop the MICFIL
# leg (the playback verdict still stands) rather than fake it.
MICFIL_CAP=1
if ! "${CROSS}gcc" -O2 -Wall -I"$ALSA_SYSROOT/usr/include" \
        -o "$TMP/pcm_capture" "$HERE/pcm_capture.c" "$LASOUND" \
        -Wl,--allow-shlib-undefined 2>/dev/null; then
    echo "note: pcm_capture oracle did not build; skipping the MICFIL rate leg"
    MICFIL_CAP=0
fi

# The SPDIF player for the XCVR card - same toolchain. Drives the XCVR TX FIFO
# -> eDMA2 datapath and makes the model emit its TX-start trace, so run.sh can
# check the sample rate derived from spdif_root tracks the requested one. If it
# will not build, drop the SPDIF leg rather than fake it.
XCVR_TX=1
if ! "${CROSS}gcc" -O2 -Wall -I"$ALSA_SYSROOT/usr/include" \
        -o "$TMP/spdif_play" "$HERE/spdif_play.c" "$LASOUND" \
        -Wl,--allow-shlib-undefined 2>/dev/null; then
    echo "note: spdif_play oracle did not build; skipping the XCVR rate leg"
    XCVR_TX=0
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
# It runs at MORE THAN ONE RATE, and that is not decoration.
#
# On this board the SAI is a bit-clock SLAVE (TCR2.BCD_MSTR is clear - the WM8962
# drives BCLK/LRCLK), so the frame rate lives in the CODEC, programmed over I2C,
# and is not derivable from any SAI register: they are byte-identical at 48 kHz
# and 16 kHz. The codec decodes it and drives it to the SAI over the "rate" wire.
#
# A model that assumed 48 kHz played a 16 kHz stream three times too fast - and
# the only test that existed asked for 48 kHz, which is the one rate that cannot
# see the assumption. Asking a second question is what caught it, and is what
# keeps it caught: cut the codec->SAI rate wire and the 16 kHz case goes red with
# its tone transposed from 145 Hz back up to 436 Hz.
#
RATES=${RATES:-"48000 16000"}
# The XCVR/SPDIF leg runs on its own boots: its PCM floor is 32 kHz (so it
# cannot share the 16 kHz SAI/MICFIL rate), and driving it spdif-only keeps the
# unrelated SAI/wm8962 datapath out of its verdict. Two in-range, distinct rates
# make a hardcoded SPDIF pacer detectable.
SPDIF_RATES=${SPDIF_RATES:-"48000 32000"}
LOG=$TMP/console.log
WAVDIR=${WAVDIR:-$TMP}

fail() { echo "FAIL: $*"; echo "--- console tail ---"; tail -20 "$LOG"; exit 1; }

for RATE in $RATES; do
    echo "$RATE" > "$TMP/rate"
    OVL='myinit\npcm_play\nrate\n'
    [ "$MICFIL_CAP" = 1 ] && OVL="${OVL}pcm_capture\n"
    ( cd "$TMP" && printf "$OVL" | cpio -o -H newc \
        2>/dev/null > overlay.cpio )
    cat "$BASE_INITRD" "$TMP/overlay.cpio" > "$TMP/combined.cpio.gz"

    WAV="$WAVDIR/capture-$RATE.wav"
    rm -f "$WAV"
    TRACELOG="$TMP/trace-$RATE.log"
    rm -f "$TRACELOG"

    # Route the audio-root rate traces to the -D file so run.sh can read the
    # sample rate each model derived from its CCM clock (pdm_root / spdif_root).
    timeout -s KILL "${TMO:-240}" "$QEMU" -M imx93-11x11-evk -m 4G -display none \
        -audio "driver=wav,path=$WAV,out.frequency=$RATE" \
        -kernel "$KERNEL" -dtb "$DTB" -initrd "$TMP/combined.cpio.gz" \
        -D "$TRACELOG" \
        -d trace:imx93_micfil_capture_start,trace:imx93_xcvr_tx_start \
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

    # MICFIL leg: the PDM capture path must actually deliver a non-silent signal,
    # AND the rate the model derived from the CCM's pdm_root must equal ${RATE}.
    # A hardcoded pacer emits the same number at 48 kHz and 16 kHz; this asks
    # both questions, so it goes red if the MICFIL stops reading its clock.
    if [ "$MICFIL_CAP" = 1 ]; then
        grep -qE 'CAP\[hw:[0-9]+,0\]: PASS' "$LOG" || \
            fail "[$RATE Hz] pcm_capture did not PASS (the MICFIL -> eDMA1 capture path delivered no signal)"
        # Grep the pdm_root prefix, not a bare "sample rate", so the XCVR's own
        # rate trace can never satisfy the MICFIL assertion by accident.
        grep -qE "pdm_root [0-9]+ Hz -> sample rate ${RATE} Hz" "$TRACELOG" || \
            fail "[$RATE Hz] the MICFIL derived the wrong rate from pdm_root: $(grep -oE 'pdm_root.*sample rate [0-9]+ Hz' "$TRACELOG" | tail -1)"
    fi
done

echo "PASS: real PCM through wm8962/SAI3 -> eDMA3 cyclic, verified at $RATES Hz"
[ "$MICFIL_CAP" = 1 ] && \
    echo "PASS: MICFIL PDM capture -> eDMA1, sample rate derived from CCM pdm_root, verified at $RATES Hz"

# ---------------------------------------------------------------------------
# XCVR / SPDIF leg: own spdif-only boots, so the wm8962/SAI datapath does not
# color the verdict and the two in-range rates (48 k, 32 k) can catch a pacer
# that stopped reading spdif_root.
# ---------------------------------------------------------------------------
if [ "$XCVR_TX" = 1 ]; then
    for RATE in $SPDIF_RATES; do
        echo "$RATE" > "$TMP/rate"
        : > "$TMP/spdif_only"
        ( cd "$TMP" && printf 'myinit\nspdif_play\nrate\nspdif_only\n' | \
            cpio -o -H newc 2>/dev/null > overlay.cpio )
        cat "$BASE_INITRD" "$TMP/overlay.cpio" > "$TMP/combined.cpio.gz"
        TRACELOG="$TMP/trace-spdif-$RATE.log"; rm -f "$TRACELOG"

        timeout -s KILL "${TMO:-240}" "$QEMU" -M imx93-11x11-evk -m 4G -display none \
            -kernel "$KERNEL" -dtb "$DTB" -initrd "$TMP/combined.cpio.gz" \
            -D "$TRACELOG" -d trace:imx93_xcvr_tx_start \
            -append "console=ttyLP0,115200 cpuidle.off=1 rdinit=/myinit ignore_loglevel" \
            -serial "file:$LOG" -serial null >/dev/null 2>&1 || true

        grep -qE 'SPDIF\[hw:[0-9]+,0\]: PASS' "$LOG" || \
            fail "[SPDIF $RATE Hz] spdif_play did not PASS (the XCVR -> eDMA2 TX path did not drain the stream)"
        grep -qE "spdif_root [0-9]+ Hz -> sample rate ${RATE} Hz" "$TRACELOG" || \
            fail "[SPDIF $RATE Hz] the XCVR derived the wrong rate from spdif_root: $(grep -oE 'spdif_root.*sample rate [0-9]+ Hz' "$TRACELOG" | tail -1)"
    done
    echo "PASS: XCVR SPDIF TX -> eDMA2, sample rate derived from CCM spdif_root, verified at $SPDIF_RATES Hz"
fi
