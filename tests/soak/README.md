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
| MAIN     | `imx93-11x11-evk`                | full audio surface (see below) + **Ethos-U65 NPU inference** (bit-exact class check, boots the M33 + ethos firmware) + **PXP G2D** 2D copies + LPI2C + SD storage (write/read/md5 verify) + slirp networking, all concurrent |
| CAMERA   | `imx93-11x11-evk-mt9m114`        | V4L2 capture, parallel-CSI path (mt9m114 → pcsi → ISI) |
| MIPICAM  | `imx93-11x11-frdm-ov5640`        | V4L2 capture, MIPI CSI-2 path (ov5640 → dw-mipi-csi2 → ISI) |
| DISPLAY  | `imx93-11x11-evk-rm67199`        | LCDIFv3 → DSI → rm67199 panel scanout (fb0 fill + page-flip DMA) |
| FLEXIO   | `imx93-11x11-evk-flexio-i2c`     | tmp105 round-trips over FlexIO-as-I2C (also stresses the defer-shift anti-storm fix) |
| I3C      | `imx93-11x11-evk-i3c`            | wm8962 moved onto the Silvaco I3C1 bus (legacy-I2C target); continuous PCM playback over I3C |

### MAIN audio surface

The MAIN block drives the whole audio datapath **continuously and
concurrently**: wm8962/SAI3 **playback** and **capture** (SAI-RX) and
XCVR/**SPDIF** playback all on eDMA2, plus **MICFIL** PDM capture on eDMA1 —
every eDMA channel busy at once.

This used to be impossible: the eDMA serviced the *first* armed cyclic channel
on any request, so the SAI3 + XCVR streams sharing eDMA2 starved each other and
MAIN had to run one eDMA2 stream at a time. That is fixed — `edma_dma_request`
now routes each request to the channel whose `CHn_MUX` selected its source (and
the SAI exposes separate TX/RX request lines), so all four streams run together.
The fix is in the shared `imx93_edma.c` + `imx93_sai.c` (byte-identical with the
imx91 tree); the per-SoC source-id wiring lives in the board.

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
