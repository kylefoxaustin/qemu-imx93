# i.MX93 max-concurrency torture shakeout

Beats on the whole machine at once: the Wayland desktop *and* an animating
client run while the full audio surface plays and captures, both A55s are
saturated, and SD storage + network churn. The bugs that survive isolated tests
live at the seams - IRQ interleave, eDMA / bus / DRAM-bandwidth contention - and
only fire when everything hammers simultaneously. `soak36` covers each datapath
but runs the display in its own block; this one drives the display
*concurrently* with the compute.

The Ethos-U65 NPU is deliberately left out of the concurrent set - its eIQ
delegate boots the Cortex-M33 on demand, and that bring-up is a known
load-sensitive guest-side race that intermittently wedges the whole guest under
the desktop (see "Known issues"). The NPU datapath is covered standalone by
`tests/ethosu-infer` and `tests/ethosu-rpmsg`.

## What runs concurrently

| Datapath        | Workload                                                      |
|-----------------|--------------------------------------------------------------|
| Display         | Weston desktop + `weston-flower` (LCDIFv3 -> DSI -> scanout)  |
| Audio (4x eDMA) | wm8962/SAI3 play + capture, SPDIF (XCVR), MICFIL PDM capture  |
| CPU             | niced `dd`/`md5sum` loops on both A55s                        |
| Storage         | `dd` + `sync` + `md5` write/verify on the SD rootfs (uSDHC)   |
| Network         | IP stack over loopback (see "Known issues" re: the FEC)      |

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
(see `tests/weston-imx93`) and, for the audio streams, ALSA headers + the
on-target `libasound`. Point the env vars in `run.sh` at them, then:

    DURATION=600 ./run.sh

Prints a per-datapath summary and `RESULT: PASS/FAIL`. Screendumps land in
`$OUTDIR`; the guest serial log (with any RCU stall / soft-lockup) is saved to
`$OUTDIR/serial.log`.

## Known issues

- **Ethos-U M33 boot under the desktop.** The eIQ delegate boots the Cortex-M33
  on `/dev/ethosu0` open, and that M33 / ethos-firmware rpmsg bring-up is a
  known load-sensitive *guest-side* race (the same one that hangs the trivial
  synthetic micro-ops). On the `core-image-weston` desktop it intermittently
  RCU-stalls *during the M33 boot* and wedges the entire guest - it happens even
  with the M33 booted first in calm (no other load, FEC link down), so it isn't
  fixable by staging or by quieting the rest of the machine, and it isn't a QEMU
  bug. Because a lost boot takes down the whole guest (not just the NPU), the
  NPU is left out of this concurrent set entirely; it's exercised standalone by
  `tests/ethosu-infer` / `tests/ethosu-rpmsg`, where the M33 boots reliably on
  the lighter `core-image` initramfs.

- **FEC link up + M33 boot.** Backing the FEC with `-nic user` makes the above
  M33-boot wedge fire much more readily (the active link concurrent with the
  rpmsg bring-up is enough on its own). The FEC datapath itself is fine
  standalone (DHCP + ping the slirp gateway at 0% loss). QEMU backs the FEC with
  a default user-net NIC even when no `-nic` is given, so the harness passes
  `-nic none` to keep the FEC link down; the network workload then runs over
  loopback, exercising the IP stack but not the imx.enet TX/RX path.
