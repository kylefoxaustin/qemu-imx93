# i.MX93 max-concurrency torture shakeout

Beats on the whole machine at once: the Wayland desktop *and* an animating
client run while the Ethos-U65 NPU loops MobileNet, the full audio surface
plays and captures, both A55s are saturated, and SD storage + network churn.
The bugs that survive isolated tests live at the seams - IRQ interleave, eDMA /
bus / DRAM-bandwidth contention, and A55<->M33 scheduling - and only fire when
everything hammers simultaneously. `soak36` covers each datapath but runs the
display in its own block; this one drives the display *concurrently* with the
compute.

## What runs concurrently

| Datapath        | Workload                                                      |
|-----------------|--------------------------------------------------------------|
| Display         | Weston desktop + `weston-flower` (LCDIFv3 -> DSI -> scanout)  |
| NPU + M33       | `benchmark_model` mobilenet loop via the eIQ delegate        |
| Audio (4x eDMA) | wm8962/SAI3 play + capture, SPDIF (XCVR), MICFIL PDM capture  |
| CPU             | niced `dd`/`md5sum` loops on both A55s                        |
| Storage         | `dd` + `sync` + `md5` write/verify on the SD rootfs (uSDHC)   |
| Network         | ping over the user-net gateway (loopback fallback)           |

## How it works (sudo-free)

The desktop lives on the systemd `core-image-weston` rootfs, which can't be
edited without root, so the harness drives it over the **serial console**:

- Boot `core-image-weston.wic` as the SD rootfs with an external `-kernel`,
  a 9p **overlay** carrying the workload assets + `launcher.sh`, and serial +
  monitor unix sockets.
- Log in as `root` on serial (type char-by-char - a whole-line write trips the
  line discipline and doubles characters) and kick `sh /mnt/launcher.sh &`.
- Each workload bumps a counter in `overlay/progress/<name>`; the host reads
  those straight from the shared 9p dir (no serial parsing).

## Health oracle

Judged from the **guest-side counters**, which are robust to host load:

- every workload counter must keep advancing (a frozen counter = a wedged
  datapath);
- display liveness uses `vblank` (the `imx-lcdifv3-crtc.0` scanout IRQ from
  `/proc/interrupts` advancing = frames are leaving the display) plus `disp`
  (the compositor + client staying alive);
- a live serial scan for `Oops` / `Internal error` / `Kernel panic`.

QEMU-monitor screendumps are taken best-effort for visual confirmation, but the
monitor is too slow to capture every frame under full load, so the verdict does
**not** depend on them.

## Running

Needs external (non-repo) artifacts - a `use-g2d=false` `core-image-weston.wic`
(see `tests/weston-imx93`), the eIQ delegate stack + a vela MobileNet, the
Ethos-U firmware, and (for audio) ALSA headers + the on-target `libasound`.
Point the env vars in `run.sh` at them, then:

    DURATION=600 ./run.sh

Prints a per-datapath summary and `RESULT: PASS/FAIL`. Screendumps land in
`$OUTDIR`.
