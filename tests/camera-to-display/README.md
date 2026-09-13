# camera-to-display — a picture in over the CSI, out on the HDMI display

The i.MX93 vision **transport** path, end to end, with no ISP in it:

```
smart camera (already-developed YUYV)   the ISI host frame source
  -> parallel-CSI / MIPI receiver       hw/display/imx93_isi.c
  -> ISI -> DRAM capture buffer         DMA, byte-for-byte
  -> V4L2 DQBUF  (v4l2_to_fb)
  -> /dev/fb0 -> LCDIFv3 -> adv7535     the HDMI monitor, 1920x1080
```

**"Smart camera"** means the sensor emits developed YUV rather than Bayer, so no
ISP is in this path. That is the point: it isolates the transport, so a failure
here cannot hide behind image processing. Flowing raw Bayer through a real
debayer is the next step, not this one.

## One device tree, no splice

Unlike the i.MX95 — whose camera and panel must be spliced into the base dts by
hand — the i.MX93 needs no dtb surgery here. The BSP's base EVK dtb already
drives the **adv7535 HDMI bridge**, and the camera variant is "base + camera", so
`imx93-11x11-evk-mt9m114.dtb` alone carries **both** the mt9m114 sensor
(`/dev/video0`) and the display (`/dev/fb0`). No overlay, no `fdtoverlay`, no
panel attach — one prebuilt dtb, both ends of the path.

## Two independent proofs, because either alone is weak

1. **Bytes.** The guest hashes the captured frame (FNV-1a); the host hashes what
   it fed in, at the same stride. Equal hashes mean the image crossed the
   pipeline *intact*, not merely that something arrived.
2. **Pixels.** A `screendump` of the display is correlated against the source
   image. A correct hash with a black screen is still a broken scanout path, and
   only the second check catches it.

Result: `r=0.9986`, luma MAE 0.5, the captured hash equal to a staged frame.

> **The hash does not certify the frame; it certifies the bytes you chose to
> feed it.** With a 3840-byte stride carrying 2560 bytes of pixel, a hash over
> the wrong extent comes out clean while the image is sheared. The check is
> sound and its *scope* is the thing that silently moves.

## The ISI falls back to its gradient *silently* — so the host hash is the instrument

With a frames source configured but a per-frame read that comes up short — the
staged frames the wrong size, the geometry misjudged, the file truncated —
`hw/display/imx93_isi.c` serves its synthetic moving gradient and raises the
frame-stored IRQ **with no warning**. The capture then looks perfectly healthy:
frames arrive, they even *vary* (the gradient moves). Nothing in the guest can
tell you the ISI ignored your photograph.

That is exactly why proof #1 hashes the guest capture against the **host-staged**
frames rather than trusting that a frame arrived. A gradient substitution hashes
to none of the staged frames and fails here, honestly. (When this test was first
wired at the wrong geometry it did precisely this, and the hash check caught it —
the failure that taught the note.) The host-side hash is load-bearing, not a
belt-and-suspenders spare.

## Running it

```sh
QEMU=…/qemu-system-aarch64 ./run.sh          # uses scene.png
SRC_IMG=~/my-photo.jpg ./run.sh              # or any image you like
SHOT=/tmp/panel.ppm ./run.sh                 # keep the screendump
HOLD=1 ./run.sh                              # hold the frame up (board-farm pane)
```

Needs the usual operator-supplied pieces (kernel `Image`, the mt9m114 base dtb,
the base initramfs, a cross gcc) plus python3-Pillow; it SKIPs cleanly without
them.

## Notes worth keeping

- The capture node is **multiplanar** (`V4L2_CAP_VIDEO_CAPTURE_MPLANE`). The
  single-planar `G_FMT` returns `EINVAL` — the `_MPLANE` API is required.
- The pipeline negotiates **1280×720 YUYV with a 3840-byte line stride**. The
  ISI computes its per-frame read as `stride × height = 2,764,800` bytes;
  `mkframe.py` honours whatever stride V4L2 reports rather than assuming packed
  lines, and leaves the padding zeroed. (The camera bring-up test's `640×480`
  example is a stale illustration — trust the `CAPTURE …` line the client prints.)
- The media graph's links must be enabled and the sensor format propagated onto
  every crossbar sink before `STREAMON` validates. `tests/camera-imx93/v4l2_cap.c`
  already does that in `cap` mode, so this test runs it first rather than
  duplicating the logic.
- **Stage several distinct frames, not one.** Fed a single file the ISI rewinds
  and re-serves it, so every captured frame is byte-identical and a run that
  captured one frame and re-read it three times produces an identical log at
  `r=1.0000` — a statement about transport only. This test stages three frames
  with a small per-frame stamp and *requires* the captured hashes to vary, so
  the number means per-frame capture, not just that the wire works.
- The frame is centred on the display (1280×720 at +320+180 of 1920×1080) and
  stays that way deliberately. Scaling to full screen would put a resample back
  between capture and scanout — the very class of step that can hide a transport
  fault by smearing it into something that still looks like a photograph — and
  it would also destroy the known-black surround that makes the "99.4%
  non-black" check meaningful rather than tautological.
- A board-farm pane that polls (a screendump on its own timer, which the guest
  cannot trigger) will **miss** a blit that lands and exits inside that window —
  and miss it silently, since a black pane looks exactly like a dead camera path.
  `HOLD=1` holds the final frame for that case.
