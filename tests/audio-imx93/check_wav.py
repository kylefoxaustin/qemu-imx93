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

# What pcm_play plays (see pcm_play.c). The square wave flips sign every 55
# frames AT THE PLAYBACK RATE, so its tone scales with that rate: 436 Hz at
# 48 kHz, 145 Hz at 16 kHz. That is the whole point of running more than one
# rate - a SAI that ignores the guest's rate and plays everything at 48 kHz
# renders the 16 kHz stream three times too fast, and its tone comes back at
# 436 Hz instead of 145 Hz. One operating point cannot see that.
SRC_HALF_PERIOD = 55                       # frames between sign flips
SRC_PEAK = 8000
SRC_SECONDS = 1

# The backend opens and closes around the stream, so allow a little loss at the
# edges; and resampling shifts the measured period slightly.
MIN_SIGNAL_FRACTION = 0.90
TONE_TOLERANCE = 0.05

# Run-length structure (91emulator's method): peak/tone/duration are SUMMARY
# statistics - a scattered ~0.4% sample loss (the audio_cb advance-by-chunk bug)
# moves none of them past tolerance, so it passes. The run LENGTHS do not lie:
# with the wav rate pinned (run.sh passes out.frequency, so no 48k->44.1k
# resample), every interior half-period is exactly SRC_HALF_PERIOD frames; a
# dropped sample cuts one run short. Assert the fraction of off-length runs.
RUN_TOLERANCE = 0          # exact: a dropped sample makes a 54-run, must count
RUN_OFF_FRACTION = 0.01    # baseline is deterministically 0% (rate pinned); >1% = a real drop


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
    # The rate the GUEST asked for - not the rate of the capture, which QEMU's
    # mixer resamples. The tone is what carries across that resampling.
    play_rate = int(sys.argv[2]) if len(sys.argv) > 2 else 48000
    src_tone = play_rate / (2 * SRC_HALF_PERIOD)

    chans, rate, samples = parse_wav(path)
    left = samples[0::chans] if chans else samples

    nonzero = [s for s in left if s != 0]
    peak = max((abs(s) for s in left), default=0)

    # Sign flips give the square wave's half period, hence its frequency; the
    # run LENGTH between flips is the structural signal a dropped sample breaks.
    flips = 0
    prev = 0
    run = 0
    runs = []
    for s in nonzero:
        sign = 1 if s > 0 else -1
        if prev and sign != prev:
            flips += 1
            runs.append(run)
            run = 0
        run += 1
        prev = sign
    half_period = len(nonzero) / flips if flips else 0
    tone = rate / (2 * half_period) if half_period else 0

    # Expected run length: SRC_HALF_PERIOD when the capture is at the play rate
    # (rate pinned, no resample); scaled by the resample ratio otherwise.
    exp_run = SRC_HALF_PERIOD * rate / play_rate
    interior = runs[1:-1]                       # drop the partial end runs
    off = sum(1 for r in interior if abs(r - exp_run) > RUN_TOLERANCE)
    off_frac = off / len(interior) if interior else 1.0

    stats = (f"played {play_rate} Hz -> captured {len(left)} frames, {chans}ch, "
             f"{rate} Hz, peak {peak}, {len(nonzero)} carrying signal, "
             f"tone {tone:.1f} Hz (want {src_tone:.1f}), "
             f"{off_frac * 100:.1f}% runs off {exp_run:.0f}")

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

    lo = src_tone * (1 - TONE_TOLERANCE)
    hi = src_tone * (1 + TONE_TOLERANCE)
    if not (lo <= tone <= hi):
        fails.append(f"tone is {tone:.1f} Hz, expected {src_tone:.1f} Hz for a "
                     f"{play_rate} Hz stream: the SAI played it at the wrong "
                     f"rate (a model that assumes 48 kHz plays 16 kHz 3x fast)")

    if off_frac > RUN_OFF_FRACTION:
        fails.append(f"{off_frac * 100:.1f}% of half-period runs are off the "
                     f"expected {exp_run:.0f} frames (allowed {RUN_OFF_FRACTION * 100:.0f}%): "
                     f"the datapath dropped samples inside the stream - a loss that "
                     f"peak/tone/duration cannot feel but the run structure does")

    if fails:
        print(f"capture: {stats}")
        for f in fails:
            print(f"  FAIL: {f}")
        sys.exit(1)

    print(f"capture verified: {stats}")


def _selftest():
    """Prove the run-length check catches a scattered drop that peak/tone/
    duration wave through - deterministically, without a guest boot. Build a
    clean +/-8000 square wave (55-frame runs) and a copy with 1-in-250 frames
    dropped; the clean one must pass, the dropped one must fail on run structure.
    """
    def synth(drop_1_in=0):
        rate = 48000
        frames, sign, run = [], 1, 0
        for _ in range(rate):
            if run >= SRC_HALF_PERIOD:
                sign = -sign
                run = 0
            frames.append(sign * SRC_PEAK)
            run += 1
        if drop_1_in:
            frames = [f for j, f in enumerate(frames) if j % drop_1_in]
        data = b"".join(struct.pack("<hh", f, f) for f in frames)   # L=R stereo
        return (b"RIFF" + struct.pack("<I", 36 + len(data)) + b"WAVE" + b"fmt "
                + struct.pack("<IHHIIHH", 16, 1, 2, rate, rate * 4, 4, 16)
                + b"data" + struct.pack("<I", len(data)) + data)

    import subprocess
    ok = True
    for name, drop, want_pass in (("clean", 0, True), ("dropped", 250, False)):
        p = f"/tmp/check_wav_selftest_{name}.wav"
        open(p, "wb").write(synth(drop))
        r = subprocess.run([sys.executable, __file__, p, "48000"],
                           capture_output=True, text=True)
        passed = r.returncode == 0
        tag = "PASS" if passed == want_pass else "FAIL"
        if tag == "FAIL":
            ok = False
        line = [l for l in (r.stdout + r.stderr).splitlines() if "runs off" in l]
        print(f"  {tag}  {name:8s} -> {'accepted' if passed else 'rejected'}  "
              f"{line[0].split('->')[-1].strip() if line else ''}")
    print("  --- a 0.4% drop is invisible to tone/peak/duration, caught by runs ---"
          if ok else "  --- SELFTEST FAILED ---")
    return 0 if ok else 1


if __name__ == "__main__":
    if "--selftest" in sys.argv:
        sys.exit(_selftest())
    main()
