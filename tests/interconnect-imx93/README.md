# interconnect tier — pass real data between i.MX 93 instances

Mission #5 of the board-farm directive: the emulated boards must **hook up and
pass real data** over their links (ethernet, USB, SPI, UART). This harness proves
the i.MX 93 does — and in the exact shape Holobench wires links into a lab
(a QEMU socket bridge between two instances), so the 93 is drop-in lab-ready.
It mirrors the i.MX 91 harness (`tests/interconnect-imx91/`); the two SoCs share
the LPUART model and the LPUART2 address, so the links are host-agnostic across
the i.MX 9 family.

```sh
tests/interconnect-imx93/run-uart.sh    # two instances, LPUART2 <-> socket <-> LPUART2
```

## UART link (`run-uart.sh`)

Two i.MX 93 guests, each board's **LPUART2** (`serial@44390000`, `/dev/ttyLP1`)
wired to a shared unix-domain chardev socket — one QEMU listens
(`-chardev socket,...,server=on`), the other connects (`server=off`). LPUART1
(`/dev/ttyLP0`) stays the console; the second `-serial chardev:ul` maps to
LPUART2 (`serial_hd(1)`). This is the RS-232-style board-to-board link a
developer would solder between two EVKs, modelled end to end.

The oracle is real data movement: the sender writes a fixed payload to
`/dev/ttyLP1`; the receiver reads it and checks it **byte-for-byte**. A pass
means the payload actually traversed guest A → LPUART2 → socket bridge →
LPUART2 → guest B:

```
LINK:PASS:recv:got byte-exact [IMX93-UART-LINK-payload-0123456789]
PASS: payload crossed LPUART2<->socket<->LPUART2 byte-exact between two i.MX 93 guests
```

LPUART2 is `status = "disabled"` in the stock EVK dtb, so the harness flips it to
`"okay"` with `dtc` before booting (needs the kernel-build `dtc`; set `DTC=` if
elsewhere). Otherwise it `SKIP`s cleanly.

### DMA-mode RX (the model work behind this)

The real `fsl-lpuart` driver does **not** poll LPUART2 in PIO for a non-console
UART — it pages RX through a **cyclic eDMA channel** (the dtb gives LPUART2
`dmas = <... rx tx>`), leaving `CTRL.RIE` clear and flushing the DMA ring to the
tty on an **IDLE** interrupt. So a received byte must be pulled by a **DMA
request**, not an RX interrupt, or it never reaches userspace.

The LPUART model services this (`hw/char/imx_lpuart.c`): on a byte arrival in
`BAUD.RDMAE` mode it pulses a `dma-req-rx` line and raises `STAT.IDLE`. The SoC
(`hw/arm/fsl-imx93.c`) wires each LPUART's `dma-req-rx` to its eDMA at the RX
source id from the EVK dtb — LPUART1/2 on eDMA1 (AONMIX), LPUART3-8 on eDMA2 —
so the eDMA pages the byte from `DATA` into its ring. TX is mem→device, which the
eDMA runs whole at channel start (writing `DATA` directly), so it needs no
request line. `DATA` accepts byte accesses (`.valid.min_access_size = 1`) for the
eDMA's byte-wide pulls. PIO-RX is unaffected: the request pulse is a no-op with
no cyclic channel armed, and IDLE is gated on `CTRL.ILIE`, which the driver sets
only in DMA-RX mode.

### Gotchas (each cost a boot)

- **`-smp 3` is mandatory.** The i.MX 93 has a fixed topology (2×A55 + 1×M33);
  QEMU rejects any other `-smp`. (The 91 is single-A55 and uses `-smp 1`.)
- **The 93 boots slower** than the 91 (more cores + M33), so the sender settles
  ~12 s then resends the payload a few times — a plain UART has no retransmit, so
  one burst before the peer is listening is simply dropped.

## Roadmap

- `run-spi.sh` — LPSPI cross-instance board-to-board (next; coordinating with the
  i.MX 91 LPSPI bridge for a shared oracle shape).
