# qemu-imx93

A QEMU machine type for the NXP **i.MX 93** SoC, targeting the **11×11 EVK**
(LPDDR4X) variant.

> **This is a fork of QEMU mainline.** The i.MX 93 work lives on the
> `imx93-dev` branch (the repository default and the upstream candidate), from
> its tip back to the upstream branch point `edcc429e9e`; the vast majority of
> the history is inherited from upstream QEMU. A `main` branch pins that
> upstream base, so `git diff main...imx93-dev` shows exactly the port. The
> upstream QEMU README is preserved at [`README.rst`](README.rst) — this file
> describes the i.MX 93-specific work.

qemu-imx93 is the **first QEMU model of the i.MX 93**. It boots stock NXP BSP
Linux to userspace on the dual Cortex-A55 cluster and brings up essentially the
whole EVK: **networking** (FEC + eQOS, both with DHCP), **SD/eMMC storage**,
**GPIO/PMIC**, the **EdgeLock Enclave** mailbox, **eDMA1/2**, a full
**LCDIFv3 → MIPI-DSI → ADV7535 → HDMI** (and LVDS) **display** you can log into
and type on, a **Weston/Wayland desktop**, **FlexCAN**, **USB host** (real
devices enumerate), **SAI3/WM8962 audio playback** (real PCM, capturable to a
`.wav`), **camera V4L2 capture** (real frames off `/dev/video0`), **expandable
I²C/SPI** (all 8 LPI2C/LPSPI + a FlexIO I²C master, `-device`-attachable beyond
the EVK), the **Cortex-M33 real-time core running real NXP firmware with working
A55↔M33 RPMsg**, and an **Ethos-U65 microNPU** with a real in-QEMU command-stream
executor that runs **bit-exact int8 inference**. Intended use cases are BSP
development, peripheral-driver development, multicore/RPMsg work,
NPU/accelerator bring-up, and CI for the above. It is not cycle-accurate.

Unlike the i.MX 95, the **i.MX 93 has no System Manager** — Linux programs the
CCM / ANATOP / SRC / power domains directly, so those blocks are modelled
functionally here rather than served by an M33 firmware over SCMI. Structural
conventions follow the upstream i.MX 8MP code (`hw/arm/fsl-imx8mp.{c,h}`); the
long-term aim is to be upstream-mergeable into QEMU mainline.

**Maintainer:** Kyle Fox ([@kylefoxaustin](https://github.com/kylefoxaustin)) (see `MAINTAINERS` for the canonical entry)

![i.MX93 booting Linux on the emulated HDMI display — dual-A55 SMP Tux logos](docs/images/hdmi-boot-logo.png)

*Real NXP BSP Linux scanned out at 1920×1080 by the emulated LCDIFv3, over the
DSI → ADV7535 → HDMI chain, on the stock EVK device tree. Two Tux logos = two
Cortex-A55 cores.*

## Quickstart

This fork **builds and runs as-is** — a plain clone lands on `imx93-dev`.

**1. Clone and build** — a standard QEMU build (see [Building](#building) for
host packages):

    git clone https://github.com/kylefoxaustin/qemu-imx93.git
    cd qemu-imx93
    mkdir build && cd build
    ../configure --target-list=aarch64-softmmu
    ninja qemu-system-aarch64
    ./qemu-system-aarch64 -M help | grep imx93     # -> imx93-11x11-evk

**2. First boot in seconds — no external artifacts.** A bare-metal LPUART
"hello" proves the machine + console without downloading anything from NXP
(needs an `aarch64-linux-gnu` bare-metal toolchain):

    cd tests/hello-imx93 && make && cd ../..
    ./build/qemu-system-aarch64 -M imx93-11x11-evk -nographic -m 2G \
        -kernel tests/hello-imx93/hello.bin      # -> "Hello from i.MX 93!"

**3. The full stack — Linux to userspace.** Booting Linux needs a kernel
`Image`, the `imx93-11x11-evk.dtb`, and a root filesystem, all built from the
NXP BSP (not redistributable, so not in the repo — see
[Required artifacts](#required-artifacts)). The easy path is
`tests/boot-imx93/run.sh` (serial console) or `tests/login-imx93/run.sh`
(interactive login on the emulated HDMI display; attach an SD card with
`-drive if=sd,file=disk.img,format=raw`). The equivalent manual invocation:

    ./build/qemu-system-aarch64 -M imx93-11x11-evk -m 4G -display none \
        -kernel <Image> -dtb <imx93-11x11-evk.dtb> -initrd <rootfs.cpio.gz> \
        -append "earlycon=lpuart32,mmio32,0x44380010 console=ttyLP0,115200 cpuidle.off=1 rdinit=/init" \
        -serial mon:stdio -serial null

Two cmdline details are load-bearing:

- **earlycon address `0x44380010`, not `0x44380000`.** The i.MX LPUART has
  VERID/PARAM/GLOBAL/PINCFG at 0x00–0x0C and BAUD at 0x10. Linux's regular
  driver applies the `reg_off = 0x10` offset to the DT base automatically;
  earlycon does not, so the cmdline address must be pre-offset.
- **`cpuidle.off=1`** is the conservative first-boot default (avoids the
  GICv3 WakeRequest gap shared by all GICv3 QEMU machines).

## Scope: what's modelled, what's deferred

QEMU SoC machines model controllers so their **Linux drivers bind and the
subsystem registers** — not so every byte reaches a host audio/video/NPU sink.
This port holds that bar, and goes past it where the data path is the point:

- **Functional data paths.** Networking (FEC + eQOS, both DHCP), storage
  (uSDHC → ext4 mount), the full display (LCDIFv3 → DSI → ADV7535 → HDMI scanout
  to `/dev/fb0`, plus LVDS), interactive input/login, CAN frame TX/RX, the
  Cortex-M33 + A55↔M33 RPMsg transport, the **in-QEMU Ethos-U65 executor**
  (a real int8 inference, bit-exact vs TFLite), **camera V4L2 capture** (real
  frames out of `/dev/video0`), **SAI3 audio playback** (real PCM, capturable to
  a `.wav`), and an **extra FlexIO I²C master** all move real data end to end.
- **Expandable beyond the EVK.** All 8 LPI2C and 8 LPSPI controllers are real and
  command-line attachable (`-device tmp105,bus=lpi2c5,…`), so the machine can
  host peripherals the reference board never wired.
- **Registration bar.** MICFIL (PDM mic) and the SAI receive path register their
  ALSA cards but don't pump real capture samples yet.
- **Deferred / not on silicon.** No 3D GPU; the 2D PXP does accelerated G2D
  copy + fill + blit + src-over blend + rotation (see below), enough for
  `use-g2d=true` Weston to composite opaque *and* alpha-blended surfaces. CSC is
  not modelled; g2d scale is rejected by the driver. FlexIO2 is an unmodelled
  stub.

The per-device tags under **What runs today** make this split explicit.

## What runs today

Stock **NXP Linux 6.12.49** boots to userspace (PID 1) on both Cortex-A55
cores, on the **stock `imx93-11x11-evk` device tree — no DT modifications**.

> **At a glance:** [`docs/validation/test-result-matrix.md`](docs/validation/test-result-matrix.md)
> is the per-IP-block test/result matrix (fidelity tier + evidence for every
> block, plus the test harnesses and their results), and
> [`docs/validation/fidelity-audit.md`](docs/validation/fidelity-audit.md)
> records the carried caveats and the honest-fault discipline. The prose below
> expands on them.

Each device below is tagged **functional** (the host driver's data path runs
end to end — data actually moves) or **brings up** (the driver binds and the
device registers / enumerates — the registration bar, no working host data path
yet).

- **SMP boot — functional.** Both A55 cores to userspace, **serial console**
  on `ttyLP0`, clean **PSCI power-off**.
- **Networking — functional.** Both NICs live with DHCP — FEC (`eth0`, reuses
  `hw/net/imx_fec.c`) and a from-scratch eQOS/dwmac4 (`eth1`,
  `hw/net/imx93_dwmac.c`).
- **Storage — functional.** SD/MMC via uSDHC from `-drive if=sd`; mounts a real
  ext4 image (`mmcblk0`).
- **Clocks / power — functional.** CCM clock roots+gates, ANATOP fractional-N
  PLLs, the MEDIAMIX power domain (genpd) and block-control GPR.
- **I²C + PMIC — functional.** LPI2C master with the board's PCA9451A PMIC and
  PCAL6524 I/O expander; regulators register and unblock uSDHC. **All 8 LPI2C
  controllers** are real (was 3) and each I²C bus is named, so peripherals attach
  at runtime — `-device tmp105,bus=lpi2c5,address=0x49` is read/written from
  Linux byte-exact. **LPSPI1–8** expose named SSI buses the same way — attach an
  SSI slave with `-device <flash>,bus=lpspi1`; an is25lp064 SPI NOR answers a
  JEDEC-ID read (`0x9d 0x60 0x17`) byte-exact through the controller.
- **I3C — functional.** I3C1 is a real Silvaco (`silvaco,i3c-master-v1`)
  controller (`hw/i3c/svc_i3c.c`). Booting the `…-i3c` DTB, which moves the
  wm8962 codec onto the I3C bus as a legacy-I²C target, the controller's I3C
  adapter registers and the codec probes over it (`wm8962 8-001a: customer id 0
  revision A`), bringing the wm8962 audio card up. (I3C2 is a logging stub — no
  DTB exercises it.)
- **FlexIO I²C — functional.** Booting the `…-flexio-i2c` DTB, the FlexIO
  shifter/timer fabric is driven as an extra I²C master (`nxp,imx-flexio` +
  `i2c-flexio`) → `/dev/i2c-8`. The model clocks the I²C handshake against a real
  QEMU I²C bus, so `-device tmp105,bus=flexio1-i2c,…` round-trips read/write. The
  shift is paced on a virtual-time timer (the driver busy-polls the shifter
  status) and gated on the prior receive byte being drained, so the
  level-triggered interrupt keeps a clean low gap and cannot storm — validated
  at 5000 back-to-back round-trips with zero stalls.
- **GPIO + ELE — functional.** GPIO controllers; **ELE** (EdgeLock Enclave) s4
  MU + responder, so the OCOTP MAC nvmem cells resolve.
- **eDMA1/2 — functional.** (`hw/dma/imx93_edma.c`) real TCD execution — `edma1`
  (AONMIX, the `edma3`-gen IP) drives the LPI2C EDID read among others; `edma2`
  (WAKEUPMIX, `edma4`-gen) paces the audio FIFOs. (Linux/DT name the instances
  `edma1`/`edma2`; "edma3/edma4" are their IP generations.)
- **Display, HDMI — functional.** Full pipeline — the LCDIFv3 controller scans a framebuffer
  out of guest DRAM; a dw-mipi-dsi host + an ADV7535 HDMI bridge (with a
  generated EDID served over I²C-DDC) satisfy the DRM stack, which sets a
  1920×1080 mode and brings up `/dev/fb0`. fbcon renders the console on the
  emulated display.
- **Display, LVDS — functional.** Booting the `…-boe-wxga-lvds-panel` DTB lights
  the second display path, LCDIFv3 → LDB → LVDS-PHY → a fixed `boe` panel at
  1280×800 (no EDID; the panel mode is fixed). Needs the adp5585 I/O expander
  (modelled) for the panel's backlight.
- **Input + interactive login — functional.** virtio-mmio transports + a virtio-keyboard
  (and tablet) let you **type in the QEMU window** onto the HDMI console; root
  logs in (the BSP image's root has no password) on both the framebuffer
  console and serial.
- **CAN (FlexCAN1/2) — functional.** On QEMU's CAN bus subsystem, the Linux `flexcan` driver
  binds and `can0` brings up; real frame TX/RX between the two controllers is
  covered by a kernel-free qtest. Attach a bus with
  `-object can-bus,id=cb -machine canbus0=cb,canbus1=cb`.
- **USB host (ChipIdea OTG1/2) — functional.** The `ci_hdrc` driver brings up both
  EHCI host controllers and real USB devices enumerate: `-device usb-kbd`
  binds as a HID input and `-device usb-storage,drive=…` attaches as a SCSI
  disk (`sda`). The stock EVK device tree's Type-C role switch is unmodelled,
  but the controller falls back to host mode, so no DT override is needed.
- **Audio — functional (full surface).** The ASoC stack registers the EVK ALSA
  cards (SAI1 bt-sco, the **WM8962** SAI3 card via a modelled codec, **MICFIL**
  PDM, and the **XCVR/SPDIF** card). All the real datapaths move PCM end to end:
  **WM8962/SAI3 playback** (SAI3 TX FIFO drains at the audio word rate, paced by
  a **cyclic eDMA2 scatter-gather** channel) and **capture** (SAI-RX), **XCVR
  SPDIF playback**, and **MICFIL PDM capture** — and they run **concurrently**
  (the eDMA routes each request to its channel by `CHn_MUX` source). Playback is
  handed to QEMU's audio backend, so `-audio driver=wav,path=out.wav` captures it
  to a real `.wav` (a played square wave comes back byte-correct). See
  `tests/audio-imx93/run.sh` (set `WAV=out.wav`).
- **Camera capture — functional.** Both CSI front-ends run end to end to **real
  frames out of `/dev/video0`**: the parallel path (`…-mt9m114` DTB: MT9M114 →
  parallel-CSI → ISI → V4L2) and the **MIPI CSI-2** path (`…-frdm-ov5640` DTB:
  ov5640 → dw-mipi-csi2 → ISI, after `modprobe ov5640`). The ISI model DMAs
  frames into the driver's ping-pong buffers and raises the frame-done IRQ; a
  V4L2 client enabling the (default-disabled) sensor link and propagating the pad
  formats streams a moving test pattern (5/5 frames, MMAP buffers, byte-checked).
  The media graph (`/dev/media0`, subdevs, two ISI `/dev/video*` nodes) registers
  as before. See `tests/camera-imx93/run.sh`.
- **Virtual camera — functional.** The ISI can scan **real host images** through
  the capture pipeline instead of the test pattern — a sensor-less "virtual
  camera". Point its `frames` property at a host path (a directory of `*.raw` or
  a file of back-to-back raw frames) and the model reads the next frame each tick
  and scans it out, looping: `-global driver=imx93.isi,property=frames,value=…`.
  Byte-exact validated (fed-frame hash == captured hash, 5/5). Useful to feed a
  sequence of images to drive the NPU or a vision pipeline. See
  `tests/camera-imx93/csi-inject-test.sh` and `tests/camera-imx93/README.md`.
- **Wayland desktop — functional.** A `core-image-weston` rootfs boots to the Weston
  compositor on the emulated display — desktop, panel/clock, and apps
  (e.g. `weston-terminal`), driven by the virtio keyboard + pointer. Software
  rendered (Mesa softpipe / pixman): the i.MX 93 has no 3D GPU. This test uses
  `use-g2d=false`; the PXP now also composites (opaque + alpha-blended) under
  `use-g2d=true` (see PXP below). See `tests/weston-imx93/run.sh`.
- **GStreamer media on the display — functional.** The i.MX 93 has **no hardware
  JPEG/video codec** (its Reference Manual has no codec block — unlike the i.MX
  95's CAST mxc-jpeg), so multimedia is pure software on the A55s. A stock
  GStreamer pipeline proves that path end to end: frames are generated/decoded in
  software and handed to `waylandsink`, which the auto-started Weston composites
  (software) onto the LCDIFv3 → HDMI scanout. `videotestsrc` puts SMPTE bars on
  screen, and a real Ogg/Theora clip is `theoradec`-decoded and played to EOS.
  `tests/gstreamer-imx93/run.sh` stages the plugins (the BSP builds them but
  installs them in no image) into a throwaway rootfs and boots it.
- **PXP 2D engine — functional (G2D copy + fill + blit + blend).** The Pixel
  Pipeline (`hw/misc/imx93_pxp.c`) executes real surface ops on the ENABLE kick —
  a legacy PS→OUT same-format **copy** (`g2d_copy`), a Store-engine constant-colour
  **fill** (`g2d_clear`), an opaque Fetch→Store **blit** (`g2d_blit`), or a
  two-channel **src-over alpha blend** (`g2d_blit` + `G2D_BLEND`: Fetch CH1
  foreground over Fetch CH0 background → Store), or a **90/180/270 rotation**
  (`FETCH_CTRL[13:12]`) — then raises the completion IRQ the driver's fence waits
  on. All are proven **byte-exact** through the whole stack — `libg2d` (imx-pxp-g2d)
  → `/dev/pxp_device` → the built-in `pxp_dma_v3` driver → the model — by a
  copy/clear/blit/blend/rotate oracle (`tests/pxp-imx93/`, plus a kernel-free
  qtest). With the op set, **`use-g2d=true` Weston composites through the PXP**
  (opaque *and* alpha-blended surfaces) instead of pixman. CSC (YUV→RGB) is not
  modelled; g2d **scale** is rejected by the driver (`unsupport 2d operation`) and
  unused by Weston, so it is not exercisable here.
- **Ethos-U65 microNPU, firmware stack — functional.** Over RPMsg: on the i.MX
  93 the NPU is driven by firmware on the Cortex-M33 (its DT node has no `reg`),
  not by Linux. Opening `/dev/ethosu0` makes the `arm,ethosu` driver boot the
  M33 **on demand** via remoteproc: Linux loads the stock NXP `ethosu_firmware`
  and issues the i.MX SiP `RPROC` SMC, which the machine services by releasing
  the M33 (see "Cortex-M33" below). The firmware then initialises the modelled
  Ethos-U65 register block (0x4a900000 — product check, soft reset, access-state
  all satisfied), comes up (`Initialize Arm Ethos-U / RPMSG_LITE is link up`),
  and brings up `rpmsg-ethosu-channel`, which Linux creates and binds — cleanly,
  no kernel oops. See `tests/ethosu-rpmsg/run.sh` (and `tests/npu-imx93/run.sh`
  for the plain driver-bind without firmware). The full A55→M33→NPU round-trip
  is exercised by `tests/ethosu-caps/` — a tiny guest tool (`ethosu_caps`) opens
  `/dev/ethosu0` and issues `CAPABILITIES_REQ`; the request crosses MU/rpmsg to
  the M33, whose firmware reads the modelled NPU's ID/CONFIG registers and
  replies, and the tool prints them back (Ethos-U65, 8 MACs/cc).
- **Ethos-U65 — real in-QEMU inference, bit-exact — functional.** Building on the
  round-trip above, `tests/ethosu-infer/` runs an *actual* neural-network
  inference end to end **inside QEMU — no host stand-in**: a guest app
  `BUFFER/NETWORK/INFERENCE/INVOKE`s a Vela-compiled int8 CNN, the M33 firmware
  runs tflite-micro and **kicks the NPU** (writes `QBASE`/`BASEP`/`CMD`), and the
  generic `hw/npu/` executor (`TYPE_ETHOS_U`, instantiated as the i.MX 93's
  Ethos-U65-256) services the kick *for real* — it parses the Vela command
  stream, DMA-marshals the IFM (NHWC + NHCWB16), mlw-decodes the weights and
  inverts the reorder into an OHWI volume, unpacks the per-channel scale/bias,
  runs int8 conv / depthwise / pool / elementwise kernels with gemmlowp/TFLM
  requant, writes the OFM back into guest memory and raises the completion IRQ
  (M33 NVIC 178). On the real BSP 3-conv + 2-maxpool model the output is
  **bit-exact against the host TFLite reference** (class 0, OFM `[118, -106]`).
  Gated by 19 unit subtests (mlw / reorder / requant / kernels vs committed
  Vela+tflite vectors) and a 12-case qtest escalation suite (single-brick →
  NHCWB16 multi-brick → depth-split → multi-IFM → depthwise → DMA-staged →
  maxpool, all bit-exact). Scope is the single-brick / single-core Ethos-U65-256
  the i.MX 93 uses; multi-core deinterleave and depth-bricked streams are not yet
  handled. Unlike the earlier host-TFLite stand-in (since removed), this is a
  genuine SoC device model and an upstream-track deliverable.
- **Cortex-M33 + A55↔M33 RPMsg — functional.** The M33 is instantiated as a
  heterogeneous core alongside the A55 cluster (its own ARMv7-M context, private
  ITCM/DTCM at the `imx_rproc` view addresses with A55-side aliases for firmware
  staging, and a secure peripheral window since the firmware runs secure). It is
  held in reset until firmware is staged, so a plain Linux boot is unaffected.
  With the stock NXP firmware loaded it **runs the real FreeRTOS image and talks
  to Linux over RPMsg**: loading the `rpmsg_lite_pingpong` remote on the M33 and
  `imx_rpmsg_pingpong` on Linux, the M33 brings its link up, announces the
  channel over MU1, Linux binds it, and ping-pong messages round-trip through
  the shared vrings (`new channel ... -> ...`, M33 prints `Link is up! /
  Sending pong...`). See `tests/m33-boot/` (bare-metal) and `tests/m33-rpmsg/`
  (full RPMsg). This is the transport the Ethos-U65 NPU inference path rides on.

![Weston/Wayland desktop with a terminal on the emulated i.MX93 display](docs/images/weston-terminal.png)

*Weston compositor on the emulated HDMI output — textured background, top panel
with clock, and a `weston-terminal` window, all software-rendered.*

![GStreamer videotestsrc rendered on the emulated i.MX93 HDMI display](docs/images/gstreamer-imx93.png)

*A stock GStreamer pipeline (`videotestsrc ! videoconvert ! waylandsink`)
software-rendered through Weston onto the emulated LCDIFv3 → HDMI scanout — the
i.MX 93 has no hardware codec, so the whole media path runs on the A55s.*

## Roadmap

Everything on the EVK's roadmap is **done** and described under "What runs
today" above: networking, storage, the full **display (HDMI + LVDS) + input**
stack, **CAN**, **USB host**, **SAI3 audio playback** (real PCM, wav-capturable),
**camera V4L2 capture** (real frames), an extra **FlexIO I²C** master,
**command-line-attachable I²C/SPI** buses, the **Cortex-M33** core with **A55↔M33
RPMsg**, the **Ethos-U65** NPU — firmware stack *and* a real in-QEMU
command-stream executor running bit-exact int8 inference — and a **Weston/Wayland
desktop**.

| Feature | What | Target |
|---|---|---|
| Upstreaming | Submit the machine + its generic-QEMU prereqs (notably the board-agnostic `hw/npu/` Ethos-U executor) to qemu-devel | next |

Each modelled block is taken to at least the bar where the Linux driver binds
and the subsystem registers its devices — matching how QEMU SoC machines model
controllers for driver bring-up. Many blocks now go *past* that bar and move
real data end to end: the **audio** surface (SAI3 playback + capture, MICFIL,
SPDIF — concurrently), the **camera/ISI** path (real V4L2 frames from both CSI
front-ends, plus a host-image "virtual camera"), **PXP** 2D (byte-exact), and
the **Ethos-U65** NPU — whose `hw/npu/` executor runs the Vela command stream
and produces bit-exact int8 inference output entirely inside QEMU. So these are
real SoC device models rather than host stand-ins.

## Required artifacts

To boot Linux to userspace you need three artifacts, all built from the
[NXP i.MX Yocto BSP](https://github.com/nxp-imx/meta-imx) (`MACHINE=imx93evk`):

| Artifact | Where from |
| --- | --- |
| Kernel `Image`            | `linux-imx`, imx defconfig (the BSP kernel) |
| `imx93-11x11-evk.dtb`     | same kernel build |
| initramfs / rootfs        | any aarch64 rootfs with `/init` (e.g. the BSP `imx-image-core`) |

The `tests/*/run.sh` scripts take `KERNEL=`, `DTB=`, `INITRD=` (and `QEMU=`)
env vars and print exactly which to set if an artifact is missing.

**Fully-OSS alternative (no NXP access required).** The machine also boots a
**vanilla mainline kernel** — an `arm64 defconfig` build of upstream Linux
(verified with 6.12.x) with the **mainline** `imx93-11x11-evk.dtb` (upstream
since ~v6.3) and a small busybox initramfs reaches a shell on `ttyLP0` in
seconds, with **zero NXP bits**. `tests/busybox-initramfs/build.sh` builds that
OSS rootfs. This mainline tuple is the basis for the upstream functional test
and a redistributable demo image; the NXP BSP is only needed for the *full* EVK
userspace (Weston, GStreamer, the vendor drivers above).

## Known limitations

- **`fsl-se … Failed to read tamper status` is benign.** The ELE itself
  registers fine (`ele-trng`, `hsm0` configured). The tamper read is an NXP SiP
  SMC (`IMX_SIP_BBSM`) normally serviced by TF-A; a `-kernel` boot has no secure
  firmware, so it returns an error. Cosmetic only — not an ELE MU defect.
- **First-boot time is dominated by initramfs decompression under TCG.** A
  ~430 MB rootfs unpacks to ~1.3 GB tmpfs (~12 s on this host, logged as the
  gap before `Freeing initrd memory`); a small busybox initramfs boots far
  faster. Not a hang.
- **No 3D GPU on silicon.** The i.MX 93 has 2D PXP but no 3D GPU. The PXP G2D
  path models copy + fill + blit + src-over blend + 90/180/270 rotation (so
  `use-g2d=true` Weston composites opaque and alpha-blended surfaces); CSC is not
  modelled and g2d scale is rejected by the driver.
- **The second adp5585 I/O expander (`2-0034`, on LPI2C3) is not modelled**, so a
  few ISP/camera board rails stay in deferred-probe — non-fatal (it gates neither
  the parallel-camera capture path nor audio, whose rails sit on the modelled
  first adp5585).
- On the framebuffer console, the shell prints a cosmetic
  `cannot set terminal process group / no job control` (controlling-tty quirk);
  commands run fine.
- Not cycle-accurate (TCG); no silicon timing is implied by any throughput.

## Architecture overview

- **2× Cortex-A55** (GICv3 / GIC-600, no ITS in the base SoC), the application
  cores running Linux. DDR at `0x8000_0000`.
- **1× Cortex-M33** real-time core, instantiated as a heterogeneous ARMv8-M
  context with its own ITCM/DTCM and a secure peripheral window. It runs real
  NXP firmware and exchanges RPMsg with Linux over MU1 + shared vrings. Unlike
  the i.MX 95's M33 this is **not** a System Manager — there is no SCMI
  indirection; Linux drives CCM/ANATOP/SRC/power-domains directly and those are
  modelled functionally.
- Real device models for everything boot/display/audio/camera/USB/M33 exercise:
  LPUART, CCM, ANATOP, MEDIAMIX (blk-ctrl GPR + SRC power slice), PXP, ELE MU,
  MU1, LPI2C + PMICs, GPIO, uSDHC, FEC + eQOS, eDMA1/2, LCDIFv3 + DSI + ADV7535,
  SAI/MICFIL/WM8962, MT9M114 + parallel-CSI + ISI, ChipIdea USB, FlexCAN, and
  virtio-mmio for input. Everything else is a logging stub.
- The NXP BSP uses its **downstream `drm/imx` drivers** (`DRM_IMX_LCDIFV3`,
  `dw-mipi-dsi`, `adv7511`), not the mainline `mxsfb`/`imx` ones — worth knowing
  when comparing register behaviour against upstream Linux.

All memory-map addresses and IRQ numbers come from the NXP BSP (the
`imx93.dtsi`), never guessed; the Reference Manual is authoritative for register
behaviour.

## Repository tour

| Path | Purpose |
| --- | --- |
| `hw/arm/fsl-imx93.c`, `include/hw/arm/fsl-imx93.h` | SoC realization: A55 cluster + Cortex-M33, GIC, device wiring, memory map, virtio-mmio, logging stubs |
| `hw/arm/imx93-evk.c`        | 11×11 EVK board file (SD attach, DTB virtio node injection) |
| `hw/char/imx_lpuart.c`      | LPUART model (console) |
| `hw/misc/imx93_ccm.c`, `hw/misc/imx93_anatop.c` | CCM clock roots/gates; ANATOP PLLs |
| `hw/misc/imx93_media_blk.c` | MEDIAMIX block-ctrl GPR + SRC power-domain slice |
| `hw/misc/imx93_pxp.c`, `hw/misc/imx93_ele.c` | PXP 2D engine (G2D copy + fill + Fetch→Store blit + src-over blend + 90/180/270 rotation + completion IRQ); ELE (EdgeLock Enclave) MU + responder |
| `hw/misc/imx_mu.c`          | Messaging Unit (A55↔M33 mailbox, peer-linked endpoints) |
| `hw/npu/ethos_u*.c`, `hw/npu/mlw/` | Generic Arm Ethos-U executor (`TYPE_ETHOS_U`): cmd-stream parse → DMA marshal → mlw decode → int8 conv/dw/pool/elementwise → OFM writeback + IRQ; instantiated as the i.MX 93's Ethos-U65-256 |
| `hw/i2c/imx_lpi2c.c`        | LPI2C master ×8 (bridges to QEMU I2C bus; per-instance `bus-name` for `-device` attach) |
| `hw/ssi/imx93_lpspi.c`      | LPSPI master ×8 (bridges to QEMU SSI bus) |
| `hw/misc/imx93_flexio.c`    | FlexIO shifter/timer fabric, driven as an I²C master onto a QEMU I2C bus |
| `hw/i2c/mt9m114.c`          | MT9M114 camera sensor (I²C) |
| `hw/gpio/imx93_gpio.c`      | GPIO controllers |
| `hw/net/imx93_dwmac.c`      | eQOS dwmac4 Ethernet (from scratch) |
| `hw/net/can/flexcan.c`      | FlexCAN controller (QEMU CAN bus) |
| `hw/dma/imx93_edma.c`       | eDMA1 / eDMA2 controllers (edma3-gen + edma4-gen IP); one-shot TCD execution + cyclic/scatter-gather for audio |
| `hw/display/imx93_lcdif.c`  | LCDIFv3 display controller + framebuffer scanout |
| `hw/display/imx93_dsi.c`    | MIPI-DSI host (dw-mipi-dsi core) |
| `hw/display/imx93_isi.c`    | ISI image-sensing interface (V4L2 capture: ping-pong frame DMA + frame-done IRQ) |
| `hw/display/adv7535.c`      | ADV7535 DSI-to-HDMI bridge (I²C) |
| `hw/audio/imx93_sai.c`, `hw/audio/imx93_micfil.c` | SAI (I²S, TX FIFO + DMA-request pacing + audio-backend capture) + MICFIL (PDM mic) |
| `hw/audio/wm8962.c`         | WM8962 audio codec (I²C) |
| `tests/qtest/flexcan-test.c` | kernel-free FlexCAN model qtest (frame TX/RX) |
| `tests/qtest/imx93-pxp-test.c` | kernel-free PXP qtest (g2d_copy blit, OFM byte-exact) |
| `tests/qtest/imx93-isi-test.c`, `imx93-sai-test.c` | kernel-free ISI capture + SAI TX-FIFO qtests |
| `tests/qtest/imx93-lpi2c-test.c`, `imx93-flexio-test.c` | kernel-free LPI2C (all 8 real) + FlexIO register qtests |
| `tests/pxp-imx93/run.sh`    | PXP G2D copy e2e (libg2d → /dev/pxp_device → driver → model) |
| `tests/hello-imx93/`        | bare-metal LPUART hello (no artifacts needed) |
| `tests/m33-boot/`           | bare-metal Cortex-M33 bring-up (blob runs, writes DTCM) |
| `tests/m33-rpmsg/`          | A55↔M33 RPMsg ping-pong with the NXP M33 firmware |
| `tests/boot-imx93/run.sh`   | boot Linux to the serial console |
| `tests/login-imx93/run.sh`  | interactive login on the emulated HDMI display |
| `tests/weston-imx93/run.sh` | Weston/Wayland desktop on the emulated display |
| `tests/gstreamer-imx93/run.sh` | GStreamer software media pipeline → waylandsink → display (no HW codec) |
| `tests/lcd-panel/`, `tests/weston/`, `tests/busybox-initramfs/` | attach a prebuilt panel dtb + a pixman-rendered Weston desktop on a zero-NXP (mainline kernel + busybox) stack — demo / board-farm tooling |
| `tests/soak/soak36.sh` | self-healing endurance soak: every datapath concurrent across many boot/power-off cycles (the 24h upstream-gate run) |
| `tests/audio-imx93/run.sh`  | SAI3/WM8962 PCM playback (cyclic eDMA → FIFO; `WAV=` captures a .wav) |
| `tests/camera-imx93/run.sh` | V4L2 camera capture (MT9M114 → CSI → ISI → real frames on `/dev/video0`) |
| `tests/camera-imx93/csi-inject-test.sh` | virtual camera: feed host images via the ISI `frames=` source, byte-exact out of `/dev/video0` |
| `tests/flexio-imx93/run.sh` | FlexIO-as-I²C round-trip (tmp105 read/write over `/dev/i2c-8`) |
| `tests/i3c-imx93/run.sh`    | I3C master: wm8962 codec probes over the Silvaco I3C bus (legacy-I²C target) |
| `tests/npu-imx93/run.sh`    | Ethos-U65 driver bind check (no firmware) |
| `tests/ethosu-rpmsg/run.sh` | Ethos-U65: Linux boots the M33 on demand, channel up |
| `tests/ethosu-caps/run.sh`  | Ethos-U65: A55→M33→NPU capabilities round-trip (fork demo) |
| `tests/ethosu-infer/run.sh` | Ethos-U65: real end-to-end inference, correct output (fork demo) |
| `tests/poweroff-imx93/`     | static PSCI power-off helper |

## Building

    mkdir -p build && cd build
    ../configure --target-list=aarch64-softmmu
    ninja qemu-system-aarch64

**Host packages (Ubuntu 22.04+).** The QEMU build plus the tests:

    sudo apt install -y \
        meson ninja-build python3 python3-venv python3-tomli \
        gcc libc6-dev pkg-config libglib2.0-dev libpixman-1-dev \
        libgtk-3-dev \
        binutils-aarch64-linux-gnu gcc-aarch64-linux-gnu

- `meson … libpixman-1-dev` — the QEMU build itself.
- `libgtk-3-dev` — the `-display gtk` window (for the interactive HDMI login).
- `binutils/gcc-aarch64-linux-gnu` — the bare-metal hello + a static initramfs.

## Smoke tests

    # machine registers
    ./build/qemu-system-aarch64 -M help | grep imx93

    # bare-metal hello (no artifacts needed)
    cd tests/hello-imx93 && make && cd ../..
    ./build/qemu-system-aarch64 -M imx93-11x11-evk -nographic -m 2G \
        -kernel tests/hello-imx93/hello.bin      # -> "Hello from i.MX 93!"

    # Linux to userspace (needs the BSP artifacts)
    KERNEL=<Image> DTB=<dtb> INITRD=<rootfs> tests/boot-imx93/run.sh

    # interactive login on the emulated HDMI display
    KERNEL=<Image> DTB=<dtb> BASE_INITRD=<rootfs> tests/login-imx93/run.sh

## Methodology & contributing

Bring-up is measure-first ("Path-C triage"): boot real Linux, read the exact
external abort / hang, map or model that peripheral, repeat. Every address and
IRQ comes from the NXP DTS/RM, never guessed. The display milestone is a good
example — the HDMI pipeline *bound* but produced no modes; tracing it
(`drm.debug`) surfaced `i2c-0: I/O Error in DMA Data Transfer`, revealing that
the i.MX LPI2C routes its EDID read through eDMA, which had to be modelled
before the EDID could be read and a mode set.

Validation is layered: kernel-free `qtest`s pin model behaviour (register/reset
semantics and byte-exact data paths), the device models pass a clean
**ASan + UBSan** sweep (zero undefined-behaviour), and a self-healing
**24-hour soak** (`tests/soak/`) drives every datapath concurrently — audio,
NPU, PXP, I3C, camera, display, storage, networking — across dozens of
boot/power-off cycles as the comprehensive endurance gate before upstreaming.
The model is also verified to boot a vanilla mainline kernel, not just the NXP
BSP.

## Milestone history

- **v0.0.1–0.0.3** — scaffold (real memory map from the DTS, GICv3, DDR,
  logging stubs); `imx.lpuart` console; CCM + ANATOP.
- **Boot to userspace** — secondary-CPU MPIDR fix, MU/LPUART map, PXP
  soft-reset model; both A55s reach `/init`.
- **Power-off + storage** — the SDHCI `SDCLK_AUTO_GATE` quirk and real uSDHC,
  so guest `poweroff` reaches PSCI and SD images mount.
- **Networking** — FEC + OCOTP wiring; the ELE MU responder (OCOTP MAC nvmem
  resolves); LPI2C + PMIC (PCA9451A) + PCAL6524 → live FEC DHCP; a from-scratch
  eQOS/dwmac4 → second NIC (`eth1`) DHCP.
- **GPIO + PMIC** — GPIO controllers and PMIC vsel presets for a clean probe.
- **Display** — LCDIFv3 + dw-mipi-dsi + ADV7535 + MEDIAMIX + eDMA1 → real
  1920×1080 HDMI scanout of the framebuffer; the dual-A55 SMP Tux logos.
- **Interactive login + input** — serial + HDMI-framebuffer login; virtio-mmio
  + virtio-keyboard/tablet so you can type in the QEMU window onto the display.
- **LVDS + Weston** — second display path (LDB → LVDS panel) and a full
  `core-image-weston` Wayland desktop (software-rendered).
- **CAN + USB** — FlexCAN on the QEMU CAN bus (qtest + live driver); ChipIdea
  USB host enumerating real `usb-kbd` / `usb-storage`.
- **Audio + camera bring-up** — SAI/MICFIL/WM8962 register all three ALSA cards;
  the MT9M114 → parallel-CSI → ISI pipeline brings up the V4L2 media graph.
- **Cortex-M33 + RPMsg** — the M33 runs the real NXP FreeRTOS firmware and
  ping-pongs RPMsg messages with Linux over MU1 + shared vrings.
- **Ethos-U65 NPU firmware stack** — Linux boots the M33 on demand (the i.MX
  SiP `RPROC` SMC, serviced by the machine), loads the stock NXP ethos firmware,
  which initialises the modelled NPU and brings up `rpmsg-ethosu-channel` — no
  kernel oops.
- **Ethos-U65 in-QEMU executor** — a real command-stream compute engine in
  `hw/npu/` (vendored Arm mlw decode, reorder inverse, scale/bias unpack, int8
  conv/depthwise/pool/elementwise kernels, gemmlowp/TFLM requant) replaces the
  host-TFLite stand-in; end-to-end inference on the BSP model is bit-exact vs
  TFLite, gated by 19 unit subtests + a 12-case qtest escalation suite.
- **GStreamer + PXP G2D** — a software GStreamer pipeline renders to the LCDIFv3
  display via waylandsink; the PXP gains the full G2D op set — copy, fill,
  Fetch→Store opaque blit, two-channel src-over alpha blend, and 90/180/270
  rotation (+ completion IRQ) — all proven byte-exact through `libg2d →
  /dev/pxp_device → pxp_dma_v3` end to end plus a kernel-free qtest, so
  `use-g2d=true` Weston composites (opaque and alpha-blended) through the PXP.
- **Functional capture, playback & expandability** — the **camera** path now
  delivers real V4L2 frames (ISI ping-pong DMA + frame-done IRQ; a media-ctl-in-C
  oracle streams 5/5 byte-checked frames). **SAI3 audio playback** moves real PCM
  (SAI-drain-paced cyclic eDMA2 scatter-gather, gated on `TCD_CSR.ESG` so the
  one-shot path is untouched) and is capturable to a `.wav`. All **8 LPI2C** + 8
  LPSPI controllers are real and `-device`-attachable, and the **FlexIO** fabric
  runs as an extra I²C master (`-device tmp105,bus=flexio1-i2c,…`, byte-exact
  round-trip) — each backed by a kernel-free qtest.
- **Upstream-clean pass** — 0 checkpatch errors/warnings, MAINTAINERS entry,
  docs; tagged releases `imx93-v1.0`/`v1.1`/`v1.2`.
- **Hardening & upstream prep** — adopted the upstream Cortex-M
  `arm_cpu_has_work` halt-reason fix and kept the M33's PSCI
  `power_state`/`halt_reason` consistent at its start/stop sites; verified the
  machine boots a **vanilla mainline kernel** (zero-NXP `arm64 defconfig`), not
  just the NXP BSP; added LCD-panel attach + a pixman Weston OSS-demo path; a
  **24-hour comprehensive soak** (every datapath concurrent — audio/NPU/PXP/I3C/
  camera/display/storage/net) ran with **0 incidents**, and the device models
  are **ASan/UBSan-clean**. Tagged releases `imx93-v1.3` through `v1.5`.

## License & credits

GPL-2.0-or-later, same as QEMU. Based on upstream QEMU; see
[`README.rst`](README.rst) and `LICENSE` for QEMU's own authorship and
licensing.

---

**Created and maintained by Kyle Fox — [@kylefoxaustin](https://github.com/kylefoxaustin).**
The first-ever QEMU port of the NXP i.MX 93.
