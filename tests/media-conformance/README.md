# i.MX93 media-conformance sweep

Points the real upstream conformance tools at the live media datapaths and lets
them flush model gaps a hand-written smoke test can't see. The existing
`display-imx93` / `camera-imx93` tests prove each pipe comes up *once*; this
hammers every ioctl, pixel format, buffer state and the streaming/page-flip
paths with the suites the kernel community uses to certify real drivers.

| Lane    | Tool (upstream)            | Datapath                              | DTB                         |
|---------|----------------------------|---------------------------------------|-----------------------------|
| display | `modetest` (libdrm)        | LCDIFv3 -> DSI -> rm67199 panel (KMS) | `imx93-11x11-evk-rm67199`   |
| camera  | `v4l2-compliance` (v4l-utils) | mt9m114 -> pCSI -> ISI -> /dev/video0 | `imx93-11x11-evk-mt9m114` |

## How it works

Each lane cross-builds its tool, stages it + a guest battery into an overlay
initramfs on the base rootfs, boots the lane's DTB once, and the battery drives
the subsystem through the tool, emitting one `MEDIA:` marker per case to the
serial console. The host tallies them into a scoreboard. For display the host
also takes a QMP screendump on `MEDIA:SHOOT` and scores the framebuffer
non-black - the visual oracle the marker stream can't carry.

Marker grammar (host greps `^MEDIA:`), aligned with the fleet soak harnesses:

    MEDIA:PASS:<name>:<detail>      function confirmed by the tool / visual oracle
    MEDIA:FAIL:<name>:<detail>      present but failed
    MEDIA:SKIP:<name>:<detail>      not applicable on this DTB/model (first-class)
    MEDIA:SHOOT:<name>              host: screendump now + score non-black
    MEDIA:BATTERY:DONE              battery finished (absence => boot/hang -> FAIL)

`SKIP` is first-class: a capture-only ISI legitimately has no tuner / audio /
VBI / DV-timings, so those v4l2-compliance sub-tests SKIP, they do not FAIL.

## Display cases (modetest over KMS)

- `drm-enumerate` / `drm-topology` - connector/CRTC/encoder/plane discovery.
- `drm-format-<FMT>` - mode-set + scanout with each plane pixel format the
  LCDIF advertises (`XR24 AR24 RG16 XB24 AB24 AR15 XR15`); each exercises a
  different LCDIFv3 format path the fbdev smoke test never touched.
- `drm-scanout` (+ `-pixels`) - hold the native mode, host screendump non-black.
- `drm-pageflip` - modetest's vblank-paced flip loop (LCDIF scanout IRQ path).

## Camera cases (v4l2-compliance over ISI)

- `cam-capture-smoke` - bring the pipe up (enable the sensor link + propagate the
  format via the `v4l2_cap` oracle, since the rootfs has no media-ctl) and grab
  5 frames.
- `cam-<IOCTL>` - one marker per v4l2-compliance sub-test (the full ioctl
  conformance pass).
- `cam-streaming-conformance` - v4l2-compliance `-s`: real STREAMON / DQBUF /
  STREAMOFF frame flow through the ISI DMA.

## Running

The conformance binaries are cross-built artifacts, not in the repo.
`build-tools.sh` fetches + cross-builds all three (libdrm `modetest`, v4l-utils
`v4l2-compliance`, and the in-repo `v4l2_cap` oracle) into `./bin` - needs an
aarch64 toolchain, meson/ninja and internet. Then `run.sh` picks them up:

    ./build-tools.sh                               # one-time: cross-build tools
    ./run.sh              # both lanes (MEDIA_BIN_DIR defaults to ./bin)
    ./run.sh display      # one lane
    ./run.sh camera

Point `MEDIA_BIN_DIR` at a prebuilt dir to skip the build.

Other paths overridable via env (`KERNEL`/`DTB`/`QEMU`/`BASE_INITRD`/`DURATION`).
Prints a per-case scoreboard + `TOTAL: n PASS  n FAIL  n SKIP`; artifacts
(per-lane console logs, markers, screendumps) land in `$OUTDIR`.

## Notes

- The base rootfs already carries `libstdc++`/`libm`/`libgcc_s`/`libc`, so the
  dynamically-linked tools run with no extra libs shipped; `libdrm` is statically
  embedded in `modetest`.
- The rm67199 panel's reset GPIO sits behind the adp5585 I/O expander, which is
  unmodelled (`adp5585 2-0034: probe ... -110`); the panel still reaches
  `connected` and scans out, so this does not block KMS. (The fbdev path the
  `display-imx93` test uses is unaffected either way.)
