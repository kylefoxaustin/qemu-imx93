#!/usr/bin/env python3
"""
Verify a capture of the square wave pcm_play sent through the SAI.

pcm_play emits a square wave of amplitude +/-8000 at 48 kHz, toggling sign every
55 frames - so a tone of 48000 / (2 * 55) = 436.36 Hz, for one second. QEMU's
audio mixer resamples that to the backend's rate before writing the wav, so the
capture is NOT a byte copy of what the guest wrote: what must survive is the
signal, not the samples.

Check the signal, on its values:

  peak == 8000          the amplitude survived - nothing scaled, clipped or
                        halved the stream
  ~1s of signal         the stream arrived in full - nothing truncated it or
                        under-ran the FIFO
  tone == 436.4 Hz      the timing survived - nothing mis-paced the eDMA or
                        clocked the SAI at the wrong rate

A "non-silent" check passes all three of those failures. An oracle that cannot
fail when the thing it describes is wrong is not an oracle.

Set MEASURE=1 to print the capture's statistics instead of asserting - use that
to ground a threshold in a real run rather than in a guess.
"""

import array
import os
import struct
import sys

# What pcm_play actually plays (see pcm_play.c).
SRC_RATE = 48000
SRC_HALF_PERIOD = 55                       # frames between sign flips
SRC_TONE_HZ = SRC_RATE / (2 * SRC_HALF_PERIOD)   # 436.36 Hz
SRC_PEAK = 8000
SRC_SECONDS = 1

# The backend opens and closes around the stream, so allow a little loss at the
# edges; and resampling shifts the measured period slightly.
MIN_SIGNAL_FRACTION = 0.90
TONE_TOLERANCE = 0.05


def parse_wav(path):
    """Read a wav QEMU wrote.

    QEMU leaves the RIFF and data chunk SIZE fields at zero unless the backend
    is torn down cleanly, so the declared sizes cannot be trusted and Python's
    wave module rejects the file outright. Take the format from the fmt chunk
    and the sample count from the actual file length instead.
    """
    raw = open(path, "rb").read()
    if raw[0:4] != b"RIFF" or raw[8:12] != b"WAVE":
        sys.exit(f"{path}: not a RIFF/WAVE file")
    chans = struct.unpack("<H", raw[22:24])[0]
    rate = struct.unpack("<I", raw[24:28])[0]
    bits = struct.unpack("<H", raw[34:36])[0]
    if bits != 16:
        sys.exit(f"{path}: {bits}-bit capture; expected S16")
    data = raw[raw.index(b"data") + 8:]
    samples = array.array("h")
    samples.frombytes(data[: len(data) // 2 * 2])
    return chans, rate, samples


def main():
    path = sys.argv[1]
    chans, rate, samples = parse_wav(path)
    left = samples[0::chans] if chans else samples

    nonzero = [s for s in left if s != 0]
    peak = max((abs(s) for s in left), default=0)

    # Sign flips give the square wave's half period, hence its frequency.
    flips = 0
    prev = 0
    for s in nonzero:
        sign = 1 if s > 0 else -1
        if prev and sign != prev:
            flips += 1
        prev = sign
    half_period = len(nonzero) / flips if flips else 0
    tone = rate / (2 * half_period) if half_period else 0

    stats = (f"{len(left)} frames, {chans}ch, {rate} Hz, peak {peak}, "
             f"{len(nonzero)} carrying signal, tone {tone:.1f} Hz")

    if os.environ.get("MEASURE"):
        print(f"capture: {stats}")
        return

    fails = []
    if peak != SRC_PEAK:
        fails.append(f"peak is {peak}, expected exactly {SRC_PEAK}: the "
                     f"datapath scaled or clipped the samples")

    want_frames = rate * SRC_SECONDS
    if len(nonzero) < want_frames * MIN_SIGNAL_FRACTION:
        fails.append(f"only {len(nonzero)} frames carry signal, expected about "
                     f"{want_frames}: the stream was truncated or under-ran")

    lo = SRC_TONE_HZ * (1 - TONE_TOLERANCE)
    hi = SRC_TONE_HZ * (1 + TONE_TOLERANCE)
    if not (lo <= tone <= hi):
        fails.append(f"tone is {tone:.1f} Hz, expected {SRC_TONE_HZ:.1f} Hz: "
                     f"the stream was mis-paced or clocked at the wrong rate")

    if fails:
        print(f"capture: {stats}")
        for f in fails:
            print(f"  FAIL: {f}")
        sys.exit(1)

    print(f"capture verified: {stats}")


if __name__ == "__main__":
    main()
