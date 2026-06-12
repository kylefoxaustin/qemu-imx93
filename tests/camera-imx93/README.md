# i.MX93 camera / CSI capture tests

The i.MX93 ISI (Image Sensing Interface) model is a functional V4L2 capture
device: the guest's `imx8-isi` driver brings up `/dev/video0`, and the model
DMAs frames into the driver's queued buffers, raising the frame-stored IRQ per
frame. Two front-ends route into it — parallel CSI (mt9m114) and MIPI CSI-2
(ov5640 → dw-mipi-csi2) — and either way frames flow out `/dev/video0`.

## Tests

- **`run.sh`** — boots the mt9m114 parallel-CSI device tree, cross-builds the
  V4L2 capture oracle (`v4l2_cap.c`), brings the media graph up (the media-ctl
  link-enable + format-propagation dance), and streams MMAP buffers. Expects
  `CAMERA-CAP[/dev/video0]: PASS (N frames)`.
- **`csi-inject-test.sh`** — byte-exact test of the **host frame source** (the
  virtual camera, below): feeds known images in and proves the exact bytes come
  out of `/dev/video0`.

## Virtual camera — feed host images into the CSI pipeline

By default the ISI synthesises a moving test pattern (no real sensor data). To
push **real images** through the capture path instead — for example to drive the
NPU or a vision pipeline with a fixed sequence of frames, with no camera and no
silicon — point the ISI's `frames` property at a host path:

```
-global driver=imx93.isi,property=frames,value=/path/to/frames
```

The value is either:
- a **directory** of `*.raw` frames (read in sorted name order: `frame000.raw`,
  `frame001.raw`, …), or
- a **single file** of back-to-back raw frames.

Either way the model cycles through the frames, looping at the end, scanning one
out per frame interval. The frame source is read straight at each frame tick
(whole-frame reads), so it sustains full capture rate with no streaming or
flow-control overhead. It is DTB-agnostic: the ISI is a fixed SoC block every
capture path funnels through, so this works for any device tree that streams
through the ISI (mt9m114, ov5640, or a custom one).

### Frame format

Frames must be **raw and packed** in the pixel format the guest's V4L2 client
negotiates: `width * height * bytes-per-pixel`, with no row padding (the ISI
applies the output pitch when it writes to guest memory). Discover the geometry
from the oracle's `S_FMT` line — e.g. `fmt 640x480 fourcc=YUYV … bpl=3840` means
width 640, height 480, bytes-per-pixel = bpl/width.

Produce raw frames from real images with ffmpeg (no model dependency):

```sh
# one PNG -> one raw frame at the negotiated geometry/format
ffmpeg -i image.png -vf scale=640:480 -f rawvideo -pix_fmt yuyv422 frame000.raw

# a video/GIF -> a numbered directory of frames
ffmpeg -i clip.mp4 -vf scale=640:480 -f rawvideo -pix_fmt yuyv422 frame%03d.raw
```

(Match `-pix_fmt` to the negotiated fourcc; if the guest negotiates a different
size or format, scale and convert to match.)

### Attaching a sensor on a custom device tree

The capture graph only links if a sensor subdev exists where the device tree
expects it. The EVK camera DTBs get their sensor auto-instantiated by the board
model; a **custom** DTB attaches one on any of the named `lpi2c` buses:

```
-device ov5640,bus=lpi2c5,address=0x3c
```

(`ov5640` and `mt9m114` are both `-device`-creatable.) A brand-new sensor type
needs a small register-file model like `hw/i2c/ov5640.c`; the frame *data*
still comes from the ISI `frames` source, independent of the sensor.
