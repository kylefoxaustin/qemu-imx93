# i.MX93 QEMU soak / endurance harnesses

Long-running stress harnesses that exercise the full modelled i.MX93 datapath
surface over many hours, looking for leaks, races, IRQ storms, and DMA stalls
that only surface under sustained, concurrent load.

## Scripts

- **`soak36.sh`** — the comprehensive self-healing endurance supervisor. Runs
  detached (`setsid`) so it outlives the launching shell, loops in ~2h cycles
  until a persisted deadline (default 36h), and survives guest faults: a panic
  or early QEMU exit is logged to `incidents/`, counted, and the supervisor
  moves on. Cumulative health (boots, incidents, per-path op counts, peak RSS)
  is written to `$WORK/status` after every block; a `heartbeat` file lets an
  external watcher tell "running" from "stuck".
- **`soak36-watch.sh`** — a lightweight watchdog that reads `$WORK/status` +
  `heartbeat` and reports progress / staleness.
- **`soak-full.sh`** — a shorter multi-phase one-shot (no self-healing loop).
- **`soak.sh`** — the original minimal single-boot soak.

## What `soak36.sh` exercises (per cycle)

| Block    | DTB                              | Datapaths driven |
|----------|----------------------------------|------------------|
| MAIN     | `imx93-11x11-evk`                | full audio surface (see below) + LPI2C + SD storage (write/read/md5 verify) + slirp networking, all back-to-back |
| CAMERA   | `imx93-11x11-evk-mt9m114`        | V4L2 capture, parallel-CSI path (mt9m114 → pcsi → ISI) |
| MIPICAM  | `imx93-11x11-frdm-ov5640`        | V4L2 capture, MIPI CSI-2 path (ov5640 → dw-mipi-csi2 → ISI) |
| DISPLAY  | `imx93-11x11-evk-rm67199`        | LCDIFv3 → DSI → rm67199 panel scanout (fb0 fill + page-flip DMA) |
| FLEXIO   | `imx93-11x11-evk-flexio-i2c`     | tmp105 round-trips over FlexIO-as-I2C (also stresses the defer-shift anti-storm fix) |

### MAIN audio surface and the eDMA2 dma-req limitation

The MAIN block drives the whole audio datapath: wm8962/SAI3 **playback** and
**capture** (SAI-RX), XCVR/**SPDIF** playback, and **MICFIL** PDM capture.

There is a known **eDMA model limitation** that shapes how these are scheduled:
SAI3 and XCVR share eDMA2's single `dma-req` GPIO line, and the model advances
the *first* cyclic channel with `ERQ` set on each request pulse
(`hw/dma/imx93_edma.c:edma_dma_request`). It does not carry a source id, so it
cannot route a given peripheral's request to that peripheral's channel — one
eDMA2 stream gets full service and any *second* concurrent eDMA2 stream is
starved (its FIFO never drains, blocking `writei`/`readi`). Real hardware
routes a distinct DMA request per peripheral via the channel mux; modelling
that (per-source dma-req → CH_MUX-selected channel) is a follow-up on the
shared `imx93_edma.c` (coordinate with the imx91 tree, which shares the file).

To exercise every audio path at full bandwidth despite this, MAIN runs exactly
**one eDMA2 stream at a time**, rotating SAI3 playback → SAI3 capture →
XCVR/SPDIF playback (`AUDIO_OPS` ops each), while **MICFIL capture runs
continuously on eDMA1** (independent) throughout. Audio never stops (one eDMA2
path + MICFIL are always live) and each datapath is hammered in turn.

## Usage

```sh
# defaults: 36h, work dir /tmp/soak36, binaries from /tmp, DTBs from the BSP deploy
bash tests/soak/soak36.sh

# shorter run, custom work dir
WORK=/tmp/soak6h TOTAL=21600 bash tests/soak/soak36.sh

# watch progress
WORK=/tmp/soak36 bash tests/soak/soak36-watch.sh
```

Prerequisites (paths overridable via env — see the top of `soak36.sh`):
`qemu-system-aarch64` (built), the BSP `Image` + the five DTBs above, the
base initramfs, and the three test oracles `pcm_play`, `pcm_capture`,
`v4l2_cap` (cross-compiled from `tests/audio-imx93/` and `tests/camera-imx93/`).
