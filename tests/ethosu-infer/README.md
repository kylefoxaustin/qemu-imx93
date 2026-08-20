# Ethos-U65 end-to-end inference (fork-only)

This runs a **real, correct neural-network inference** end to end on the QEMU
i.MX93 machine — from a Linux user-space app, through the Cortex-M33 ethos
firmware, to the modelled Ethos-U65 NPU and back — and prints the (correct)
classification.

The NPU is not a stub and not a shim: the command stream is **executed in
QEMU**.

```
guest ethosu_infer ──(ioctls)──► /dev/ethosu0
   │  open() boots the M33 on demand (i.MX SiP RPROC SMC the machine services)
   │  NETWORK_CREATE(Vela model) → INFERENCE_CREATE(IFM,OFM) → INVOKE  ──(MU/rpmsg)──►
   ▼                                                                    M33 firmware
   guest reads the OFM ◄── firmware copies output ◄── NPU completion IRQ (NVIC 178)
                                                          ▲
   QEMU Ethos-U executor (hw/npu/, TYPE_ETHOS_U = "arm.ethos-u"): on the
   firmware's command-stream kick it parses the Vela command stream, marshals
   the DMA, decodes the mlw-compressed weights, and computes int8 conv /
   depthwise / pooling / elementwise / softmax with gemmlowp requantisation,
   then writes the OFM back and raises the IRQ.
```

The demo model is a tiny 16x16 int8 CNN that classifies "top half brighter"
(class 0) vs "bottom half brighter" (class 1), trained + Vela-compiled on the
host. Two sample inputs are provided; each produces the correct, distinct
result (proving it is a genuine per-input inference, not a canned value).

## Why "fork-only"

Not because the compute is missing — it isn't. The blocker is licensing: the
mlw weight decoder (`hw/npu/mlw/`) is Apache-2.0, which QEMU cannot take under
its GPL-2.0-or-later terms. That is an **NPU-only** blocker; nothing else in
this port depends on it.

## The host TFLite script is the oracle, not the implementation

`host/ethosu_host_infer.py` runs the reference int8 model on the host. It is
**not** in the data path — QEMU never calls it. It exists so the executor's OFM
can be checked against an implementation nobody involved in this port wrote:

```bash
./host/ethosu_host_infer.py host/model_int8.tflite host/sample_top.bin /tmp/golden.bin
```

The executor is bit-exact against it for every real trained workload tested.
See `docs/validation/fidelity-audit.md` for the one documented exception
(`nasnet`, where ±1 rounding can accumulate into an occasional argmax flip) and
for the opt-in `honest-fault` property that makes an uncomputable operation
fail loudly instead of returning zeros.

> **Historical note.** Until `c98a0610f15` this directory documented a
> *different* mechanism: a bring-up model (`hw/misc/imx93_ethosu.c`) that shelled
> out to the host TFLite helper on every kick, driven by `ETHOSU_HOST_INFER` /
> `ETHOSU_HOST_MODEL`. That model is gone and those variables are read nowhere in
> the tree. If you find a document claiming the command-stream engine "is not
> modelled", it predates `hw/npu/`.

## Host prerequisites (one time)

Building the model needs TensorFlow + Vela; the oracle needs tflite-runtime.
Note `numpy<2` is required (tflite-runtime's ABI):

```bash
pip install --user "tensorflow-cpu" "ethos-u-vela==4.3.0" tflite-runtime "numpy==1.26.4"
```

Neither is needed to *run* the inference — only to rebuild the model assets or
to regenerate the golden.

## Build the host assets

```bash
./host/build.sh      # -> host/model_int8{,_vela}.tflite, host/sample_{top,bottom}.bin
```

## Run

```bash
./run.sh                              # top sample  -> class 0
IFM=host/sample_bottom.bin ./run.sh   # bottom sample -> class 1
ETHOSU_TRACE=1 ./run.sh               # also dump the firmware's NPU register traffic
```

PASS = `RESULT: INFERENCE OK`, the printed `argmax class` matching the sample
(top → 0, bottom → 1), and no kernel oops. `run.sh` prints the log path; it does
not self-assert, so read the verdict.

## Files

| file | role |
|---|---|
| `ethosu_infer.c`              | guest app: full BUFFER/NETWORK/INFERENCE/INVOKE ioctl flow |
| `host/make_model.py`          | trains + int8-quantizes the demo CNN, emits sample inputs |
| `host/ethosu_host_infer.py`   | host TFLite **oracle** — an independent check on the executor's OFM, not part of the run |
| `host/build.sh`               | make_model.py + Vela → the canonical model assets |
| `run.sh`                      | stage assets, boot the on-demand path, run the inference |

The executor itself lives in `hw/npu/` (`ethos_u.c` register/ID model,
`ethos_u_cmdstream.c`, `ethos_u_compute.c`, `ethos_u_kernels.c`,
`ethos_u_requant.c`, `ethos_u_weights.c`, `ethos_u_addr.c`, `mlw/`) with unit
tests in `tests/unit/test-ethos-u-*` and device-level tests in
`tests/qtest/ethos-u-test.c`.
