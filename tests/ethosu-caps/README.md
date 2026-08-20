# Ethos-U65 capabilities round-trip (fork-only demo)

This checks the **transport** of the fork's NPU stack on the QEMU i.MX93
machine: it proves the complete A55 → M33 → NPU path works end to end, without
running any compute.

The compute engine does exist — the Ethos-U executor in `hw/npu/` runs the Vela
command stream in QEMU. For a full inference through this same path, see
`tests/ethosu-infer/`. This test is deliberately narrower: it isolates
on-demand M33 boot + rpmsg + register readback, so a transport regression can be
told apart from a compute regression.

`ethosu_caps.c` is a tiny static aarch64 guest tool. It opens `/dev/ethosu0`
and issues `ETHOSU_IOCTL_CAPABILITIES_REQ`. That:

1. makes the kernel `arm,ethosu` driver boot the Cortex-M33 **on demand** (the
   i.MX SiP `RPROC` SMC, which the machine services by releasing the M33),
2. brings up `rpmsg-ethosu-channel` over MU1/rpmsg,
3. sends the capabilities request to the M33 firmware, which reads the
   **modelled** Ethos-U65 `ID`/`CONFIG` registers (`hw/npu/ethos_u.c`),
4. replies, and the tool prints what the NPU reported.

Expected output (from the modelled register values):

```
=== Ethos-U capabilities (via /dev/ethosu0) ===
hw_id.product_major   : 1      # Ethos-U65
hw_id.arch_major_rev  : 1
...
hw_cfg.macs_per_cc    : 8
hw_cfg.cmd_stream_ver : 1
hw_cfg.custom_dma     : 0
driver 0.16.0
=== CAPABILITIES_REQ OK ===
```

## Not an upstream deliverable

`ethosu_caps` talks to a Linux char device, not to QEMU, so it is **not** part
of any upstream QEMU patch series. It lives here purely to exercise and
demonstrate the modelled NPU on the fork.

## Run

```bash
./run.sh            # auto-builds ethosu_caps if needed, then boots QEMU
```

Override paths via env: `QEMU=`, `KERNEL=`, `DTB=`, `BASE_INITRD=`. The M33
`ethosu_firmware` must be in the rootfs `/lib/firmware`. PASS = the tool prints
`CAPABILITIES_REQ OK` with no kernel oops.
