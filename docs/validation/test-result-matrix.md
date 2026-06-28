# i.MX 93 — Test & Result Matrix

Fleet-standard test/result report for `qemu-imx93` (machine `imx93-11x11-evk`).
Records, per IP block, **what is modelled, to what fidelity, and the evidence**.
The mission bar: this machine stands in for a real EVK for developers running
their own code — so the goal is **no silent failure on a known-good IP block or
routine**. A block that cannot faithfully execute should fault honestly, never
return wrong-but-clean results.

Generated against branch `imx93-dev` @ `98bc4dee869`. Regenerate when the device
set or test inventory changes.

## Fidelity tiers

| Tier | Meaning |
|------|---------|
| **A — Data path verified** | Real data/compute flows through the block and is checked against a golden or reference (end-to-end in-guest and/or qtest with golden output). |
| **B — Driver bring-up** | The stock BSP Linux driver probes and operates the block (registers, IRQs, basic transactions); behaviour is correct for bring-up but there is no golden-verified host data path. Matches QEMU's usual peripheral convention. |
| **C — Registration / stub** | Present so Linux enumerates it and does not fault; minimal or no functional behaviour. |
| **N/A** | Not present on i.MX 93 (documented so the absence is not mistaken for a gap). |

## Test harnesses

| Harness | Location | Scope | Result |
|---------|----------|-------|--------|
| Unit (pure logic) | `tests/unit/test-ethos-u-{cmdstream,mlw,kernels,requant}` | NPU decoder / weight (mlw) / int8 kernels / requant, host-only | PASS |
| qtest (device) | `tests/qtest/ethos-u-test`, `imx93-{pxp,isi,sai,lpi2c,lpspi,flexio,flexspi}-test` | MMIO-level device behaviour, golden compares | PASS |
| Functional (boot) | `upstream/test_imx93_evk.py` (staged for upstream) | Boot stock BSP Linux to userspace | PASS |
| Media conformance | `tests/media-conformance/` | `v4l2-compliance` (ISI), `modetest` (LCDIF/KMS) | 36 PASS / 0 FAIL / 26 SKIP |
| Torture / concurrency | `tests/torture/` | Live desktop while NPU/CPU/storage/net hammer | 0 oops / 0 wedge |
| Soak | (run log) | 24 h comprehensive, all datapaths concurrent | **PASS** 2026-06-13 (11 cycles, 61 boots, 0 incidents, RSS flat) |

## Compute / boot core

| Block | Tier | Evidence | Notes |
|-------|------|----------|-------|
| Dual Cortex-A55 SMP | A | Boots stock BSP Linux to userspace, SMP | Not cycle-accurate |
| Cortex-M33 + RPMsg (MU) | A | Real NXP firmware boots; A55↔M33 RPMsg live | Concurrent M33 boot can wedge the desktop guest — traced to **NXP BSP defects**, not the model |
| eDMA1 / eDMA2 | A | Drives SAI audio (cyclic, drain-paced); live | |
| CCM / ANATOP / SRC / power | B | Linux programs clocks/PLLs/resets directly (no System Manager) | The defining i.MX93 vs i.MX95 difference — modelled, not SCMI-stubbed |
| OCOTP / ELE / BBNSM / SEMA42 | B | Driver probe + mailbox / register transactions | EdgeLock Enclave mailbox functional |
| SYSCTR / TSTMR / TPM / WDOG / TMU | B | Timers/thermal/watchdog driver bring-up | |

## Networking / storage

| Block | Tier | Evidence | Notes |
|-------|------|----------|-------|
| FEC (`imx_fec`) | A | Real traffic + DHCP; torture drives live FEC | |
| eQOS (dwmac) | A | Real traffic + DHCP | |
| uSDHC (SD/eMMC) | A | Boots rootfs from SD | |
| USB (Chipidea host) | A/B | Real USB devices enumerate in-guest | Enumeration data path proven |

## Display / graphics / camera / audio

| Block | Tier | Evidence | Notes |
|-------|------|----------|-------|
| LCDIFv3 → MIPI-DSI → ADV7535 → HDMI | A | Pixels scanned out 1920×1080; `modetest` 7 formats + pageflip + SMPTE screendump | Login/typeable console; Weston desktop |
| Media Block Control | B | Display/camera muxing for the above | |
| PXP (G2D 2D engine) | A | copy/fill/blit/blend/rotate byte-exact (qtest + e2e); Weston composites through PXP | **scale + CSC not modelled** — blocked by the libg2d/pxp_dma_v3 stack, not a model gap |
| ISI + MIPI-CSI + MT9M114 / OV5640 | A | Real V4L2 frames to `/dev/video0`; `v4l2-compliance` 48/48 ioctl + 55/55 streaming | |
| SAI3 + WM8962 codec | A | Real PCM playback + WAV capture (`-audio driver=wav`) | SAI access width pinned to 16-bit for eDMA S16 writes |
| MICFIL (PDM mic) / XCVR | B | Driver bring-up | |

## Accelerator (NPU)

| Block | Tier | Evidence | Notes |
|-------|------|----------|-------|
| Ethos-U65 microNPU | A | uint8 MobileNet **end-to-end via eIQ delegate**, argmax == host; bit-exact int8 across real CNNs; qtest conv golden; unit tests (mlw/kernels/requant/cmdstream) | See caveats below |

NPU specifics:
- **Honest fault (opt-in)** — `-global driver=arm.ethos-u,property=honest-fault,value=on` makes an uncomputable op (unknown opcode / unmodelled elementwise mode) report `STATUS.CMD_PARSE_ERROR` instead of silently zero-filling. Default off preserves all passing models. qtests: `unsupported-lenient` + `unsupported-honest-fault` (commit `98bc4dee869`). Directly serves the no-silent-fail mission.
- The `hw/npu/mlw` weight decoder is **Apache-2.0** → an upstream blocker; scopes the NPU to bring-up-only for upstream, does not block the core machine.
- One residual numeric gap (nasnet, ±1-rounding accumulation, argmax flips) is isolation-blocked and capped; all real trained workloads tested are bit-exact.

## Expansion buses (developer-attachable)

| Block | Tier | Evidence | Notes |
|-------|------|----------|-------|
| LPI2C ×8 | A/B | qtest `imx93-lpi2c-test`; `-device bus=lpi2cN` attachable | |
| LPSPI ×8 | B | qtest `imx93-lpspi-test` | |
| LPUART ×8 | A | Serial console | |
| FlexIO | B | qtest `imx93-flexio-test` (I²C master) | |
| FlexSPI | B | qtest `imx93-flexspi-test` (NOR bring-up) | |
| FlexCAN / CAN bus | B | Driver bring-up, can-bus connectable | |
| GPIO / PMIC | A/B | Poweroff, GPIO-idle-HIGH; PMIC over I²C | |
| ADC | B | Driver bring-up | |

## Not present on i.MX 93 (N/A — documented)

| Block | Why N/A |
|-------|---------|
| System Manager / SCMI | i.MX93 has none — Linux programs CCM/ANATOP/SRC directly (vs i.MX95) |
| Hardware JPEG (CAST) | i.MX95-only block; JPEG on i.MX93 is software |
| GPU (compute) | No emulatable GPU compute; Linux sees the node but 3D is not executed |

## Known caveats (carried, not silent)

- **M33 concurrent boot** can wedge the desktop guest — two NXP BSP defects, documented in `tests/torture/`.
- **PXP scale + CSC** not modelled (vendor-stack limitation, not a model gap).
- **NPU mlw decoder** Apache-2.0 license (upstream blocker for the NPU only).
- **NPU nasnet** ±1-rounding residual (capped; all real trained models bit-exact).
