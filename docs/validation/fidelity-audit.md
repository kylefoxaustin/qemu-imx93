# i.MX 93 — Fidelity Audit

Companion to [`test-result-matrix.md`](test-result-matrix.md). The matrix records
*what passes*; this audit records *where the model could lie, and what we do
about it*. The governing rule for the 10k-developer board-farm mission:

> **No silent failure on a known-good IP block or routine.** A block that cannot
> faithfully execute must fault **honestly** (a guest-visible error) — never
> return wrong-but-clean results.

## Honest-fault discipline

When the model meets work it cannot compute faithfully, it has two acceptable
outcomes: compute it correctly, or **report an error the guest can see**. The
unacceptable third outcome — silently emitting wrong or zero data — is the bug
class this audit exists to eliminate.

### NPU (Ethos-U65) — opt-in honest fault — LANDED

`-global driver=arm.ethos-u,property=honest-fault,value=on` makes an
uncomputable op (unknown command-stream opcode, unmodelled elementwise mode)
set the run's failure flag, so completion reports `STATUS.CMD_PARSE_ERROR`
instead of silently zero-filling the OFM. Default off preserves every passing
model byte-for-byte. qtests `unsupported-lenient` (default, clean) +
`unsupported-honest-fault` (error bit set, IRQ still raised — never wedges).
Commit `98bc4dee869`.

**Key invariant:** the honest fault is a **non-gating status** — the IRQ is
still raised and the inference still *completes*; it just carries the error.
The engine never hangs on an unsupported op.

### NXP-accelerator fault taxonomy (fleet finding, with i.MX95)

How an NXP remote accelerator can be faulted honestly depends on **what gates
completion** in its Linux driver:

| Style | Completion gates on | Honest fault lives in |
|-------|--------------------|-----------------------|
| **Neutron** (i.MX95) | a **polled retcode** (MBOX0 == DONE) — a bad retcode *hangs* | a **side-band** field (MBOX1 `error_code`) that does not gate |
| **Ethos-U** (i.MX93) | **response arrival** (`INFERENCE_RSP` rpmsg; `ethosu_inference_rsp()` maps `rsp->status` then unconditionally wakes) | the response's **own `status` field** — in-band but **non-gating** |

Invariant either way: **the honest fault is a non-gating status channel, never
the completion signal.** (Recorded jointly with i.MX95's `docs/validation/`.)

## Carried caveats (surfaced, never silent)

These are known fidelity boundaries. Each is *documented and visible*, which is
the point — a caveat in the open is the opposite of a silent fail.

| Area | Boundary | Disposition |
|------|----------|-------------|
| **M33 concurrent boot** | Booting the M33 while the desktop guest runs can wedge the guest | Traced to **two NXP BSP defects**, documented in `tests/torture/`; not a model bug |
| **PXP G2D** | `scale` + CSC not modelled | Blocked by the libg2d / pxp_dma_v3 vendor stack (driver rejects the op), not a model gap — copy/fill/blit/blend/rotate are byte-exact |
| **NPU mlw decoder** | `hw/npu/mlw` is Apache-2.0 | Upstream-licensing blocker for the NPU only; scopes NPU to bring-up for upstream, does not block the core machine |
| **NPU nasnet** | ±1-rounding accumulation, occasional argmax flip on the deepest degenerate-int8 model | Isolation-blocked and capped; every real trained workload tested is bit-exact, every micro-op passes ≤±1 in isolation |
| **ELE soc_id** (cross-fleet note) | i.MX91's ELE reports the 93's `soc_id` (chop-down artifact) | i.MX91's item; noted here only because 93 is the derivation parent |
| **PCA9451A PMIC reset values** | The 4 BUCK4/5/6 + LDO1 voltage-select resets in `hw/i2c/imx93_i2c_regdev.c` were bring-up scaffolding at DT-range selectors, not silicon voltages — BUCK4 (EVK 3.3V SD supply) read back ~1.625V | **FIXED — full OTP register file, bit-exact to datasheet:** every non-zero PCA9451A OTP default (rev 2.1, `93_docs/PCA9451A-datasheet.pdf`, Table 17) is now seeded. Values first **derived** (fact-sheet voltages + mainline `pca9450` encoding), then **confirmed** against the datasheet (3 bucks matched to the bit; it corrected LDO1CTRL to `0xC2` = `ENMODE=11` Always-ON). Boot-verified: guest regulator sysfs reads BUCK1 0.85V / BUCK2 0.6V / BUCK4 3.3V / BUCK5 1.8V / BUCK6 1.1V / LDO1 1.8V / LDO4 0.8V / LDO5 1.8V, all `state=enabled`, 0 failures, mmc0-2 up |
| **PCA9538 I/O expander reset values** | The camera expander (`0x70` on LPI2C8) was pure memset-0 — Configuration reg read `0x00` (all OUTPUTS), not a physical POR | **FIXED — datasheet POR:** Configuration (0x03) and Output Port (0x01) seeded to `0xFF` per the TI PCA9538 datasheet (`93_docs/PCA9538-datasheet-TI.pdf`: "at power-on reset all registers return to default values"; config POR = all inputs). Same class as the PCAL6524 direction fix. Boot-verified on the mt9m114 camera DTB (camera binds, 0 IRQ-flag errors) and **qtest-proven** (`imx93-pca9538-test` drives a real LPI2C8 master transaction, reads config=0xFF; mutation-proven: neuter the seeding → reads 0x00 → fails) |
| **ADP5585 / PCAL6524 (same file)** | Off-SoC parts on the same regdev | **Checked, correct — read, not grepped:** ADP5585 `reg0=0x20` is the datasheet MAN_ID (bits [7:4]=0x2, which is the only field the adp5585 driver checks); its other registers POR to ~0, so memset-0 matches. PCAL6524's `0xFF` direction seed is a genuine pca953x POR. Neither is a fabrication — each judged against its own datasheet |
| **Off-SoC full device models** (adv7535, mt9m114, ov5640) | These have their own `reset()`, not the regdev scaffold | **Audited clean — chip-IDs sourced + driver-validated:** ov5640 returns `0x5640` and the driver errors if it differs (`ov5640_check_chip_id`, `chip_id != 0x5640`); mt9m114 returns `0x2481`, driver checks `value != 0x2481` — both are the real datasheet device-IDs and both load-bearing. The one un-sourced value, adv7535 CHIP_REVISION `0x14`, is verified **inert**: the adv7511 driver only `dev_dbg()`s it, never validates — so it is honestly labelled, not a fabrication a consumer trusts. wm8962 R27 was fixed separately this session. Sensor config registers are memset-0 but the driver writes its full init sequence, so no POR value is read before being overwritten |

## Silent-fail audit posture

- **Done:** NPU uncomputable-op path converted from silent no-op to opt-in honest fault.
- **Convention:** new device models should prefer an honest guest-visible error
  (bus error / status bit / `-d unimp` trace) over a silent no-op when they meet
  an unsupported-but-known-good routine.
- **Open:** sweep the other modelled blocks for silent no-op defaults on
  known-good paths (the same class as the NPU `default: v=0`), prioritised by
  Tier-A blocks (where a data path is claimed and a silent wrong would be worst).

### Danger-zone sweep: divide-by-zero / period-0 livelock — re-verified clean

Every division, modulo, and timer-period site in the imx93 models was traced
individually (not a bare "clean"): a register that memsets to 0 but is used as
a divisor, or a timer period of 0 that schedules a deadline already in the past
and livelocks. Each hazard site carries an **explicit** guard:

| Site | Hazard | Guard |
|------|--------|-------|
| `imx93_tpm.c` `ticks % period` | mod-by-zero | `period = (mod & 0xffff) + 1` (always ≥ 1) |
| `imx93_isi.c` `pitch / width` | div-by-zero | enclosing `if (… && width && …)` |
| `imx93_isi.c` `% n_frame_files` | mod-by-zero | `if (n_frame_files > 0)` |
| `imx93_edma.c` `/ % chan_stride` | div-by-zero | realize rejects `chan_stride < MIN` |
| `imx93_wdog.c` `toval*1000 / rate` | div-by-zero | `if (rate == 0 \|\| toval == 0) timer_del` |
| SAI / MICFIL / XCVR timer period | period-0 livelock | fixed constants, or SAI's nonzero fallback |
| `imx93_sysctr.c` compare | past-deadline re-arm | fires once + `timer_del`, no loop |

The SAI's rate-follows-codec path states the rule in the code: *"a clock that
is not running must not run infinitely fast"* — a zero rate falls back to a
sane period with a `LOG_GUEST_ERROR`, never a 0. `imx_gpt` / `imx_epit` (the
only `ptimer_set_freq` sites that take a possibly-zero freq) are not
instantiated by imx93 — it uses `imx93_tpm` + `imx93_sysctr`.

### Oracle-tightness audit: does each Tier-A test CATCH a bug, or only EXERCISE it?

A green test is only worth what it fails on. A cross-fleet SAI thread (rt1180
found a lost-sample bug; 91/93 mutation-checked their shared model) surfaced the
rule: **a test exercising a buggy path is not a test that catches the bug** — a
loose or statistical oracle stays green while the bug manifests underneath it.
Each Tier-A datapath oracle was classified, and the loose ones were proven loose:

| Datapath | Oracle | Tightness | Basis |
|----------|--------|-----------|-------|
| Interconnect (eth/UART/SPI/CAN/I2C) | byte-exact payload compare | **tight** | inspection |
| Camera (ISI) | byte-exact frame injection vs /dev/video0 | **tight** | inspection |
| Storage (SD/uSDHC) | byte-exact ext2 R/W + migration value+wire oracle | **tight** | inspection |
| NPU | argmax == host + byte-identical golden | **tight** | inspection |
| **Audio (SAI)** | peak / dominant-tone / ~1s duration (statistical) | **LOOSE** | **measured** |
| **Display (LCDIF/KMS)** | screendump structural: brightness variance + colour diversity | **tightened** | self-test-proven; R/B residual scoped |

- **Audio, measured:** reintroducing the `audio_cb` advance-by-`chunk` bug loses
  ~0.4% of frames, but the wav oracle passes — and the captured frame count has
  ~10% run-to-run jitter (wall-clock ALSA under host load), 25x the bug, so the
  oracle **cannot** be tightened to a count tolerance. `audio_cb` correctness is
  therefore code-inspection + mutation-spot-checked; a deterministic short-write
  test would need a mock backend (scoped, not built). Honest label, measured.
- **Display, was proven loose, now tightened:** the old oracle scored
  `mean = sum(pixels)/N`, **invariant under any pixel permutation** — an R/B swap,
  a torn/shifted pattern, a shuffled framebuffer all preserve the byte-sum, and a
  *solid fill* or *monochrome* frame (the garbage a wrong scanout format / base
  address produces) passed it too. Replaced with a structural verdict
  (`frame_verdict` in `media_conformance.py`): on a downsampled grid it now
  requires **brightness variance** (a uniform / solid-fill frame → ~0 → FAIL) and
  **colour diversity** (a monochrome frame → 1 colour → FAIL). Self-test-proven
  (`media_conformance.py --selftest`): a solid-grey and a green-only frame both
  PASS the old `mean ≥ 5` and both FAIL the new verdict, while a colour-bar frame
  passes. **Residual, honestly scoped:** a structure-preserving R/B swap is still
  invisible (variance and colour-count, like the mean, don't change under a global
  channel swap) — catching *that* needs a golden SMPTE signature, which needs a
  real display capture (`modetest` is absent in this env), so it is left as the
  further step rather than faked.

The four byte-exact oracles catch what they claim; audio is a measured dead-end
(oracle can't be tightened past the jitter); the display oracle is now tightened
against the solid/monochrome class with the R/B-swap residual documented.
