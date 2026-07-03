# interconnect tier — pass real data between i.MX 93 instances

Mission #5 of the board-farm directive: the emulated boards must **hook up and
pass real data** over their links (ethernet, USB, SPI, UART). This harness proves
the i.MX 93 does — and in the exact shape Holobench wires links into a lab
(a QEMU socket bridge between two instances), so the 93 is drop-in lab-ready.
It mirrors the i.MX 91 harness (`tests/interconnect-imx91/`); the two SoCs share
the LPUART model and the LPUART2 address, so the links are host-agnostic across
the i.MX 9 family.

```sh
tests/interconnect-imx93/run-eth.sh     # two instances, FEC (eth0) <-> socket <-> FEC
tests/interconnect-imx93/run-uart.sh    # two instances, LPUART2 <-> socket <-> LPUART2
tests/interconnect-imx93/run-spi.sh     # two instances, LPSPI1 <-> spi-link <-> socket <-> LPSPI1
```

## Ethernet link (`run-eth.sh`)

Two i.MX 93 guests, each board's **FEC (eth0)** bridged by a QEMU socket netdev
(`-nic socket,listen=` on the server, `-nic socket,connect=` on the client).
Static IPs on eth0 (server `192.168.7.1`, client `192.168.7.2`); the second NIC
(`-nic user` = eQOS/eth1) is unused.

The oracle ([`linktool`](linktool.c), a static TCP echo tool) proves real data
movement: the client sends a payload, the server echoes it back, the client
verifies it **byte-exact** — so the payload actually traversed guest A → FEC →
socket bridge → FEC → guest B and back:

```
LINK:PASS:server:echoed 40 bytes [IMX93-ETH-LINK-payload-0123456789-abcdef]
LINK:PASS:client:echo byte-exact (40 bytes)
PASS: payload crossed FEC<->socket<->FEC byte-exact between two i.MX 93 guests
```

The FEC (`imx.enet`) binds eth0 on the stock EVK dtb and needed no model changes.
`linktool` avoids `getaddrinfo`/NSS (raw sockets + `inet_pton`) so it links
`-static`, and the client retries `connect` for ~60 s to ride out boot/ARP
warmup. Keep `MEM >= ~1 GiB` (`MEM=2G` default): the FEC's coherent DMA pool
sits at a high physical address the smaller `-m` sizes don't cover.

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

## SPI link (`run-spi.sh`)

Two i.MX 93 guests, each board's **LPSPI1** (`spi@44360000`) master driving a
`spi-link` SSI peripheral (`-device spi-link,bus=lpspi1`), and the two spi-links
joined by a unix chardev socket — one listens, one connects. When a master
clocks a byte out (MOSI) the spi-link forwards it over the socket to the peer;
the byte clocked in (MISO) comes from a FIFO the peer feeds. So the **sender**
clocks the payload out of its master and the **receiver** clocks dummy bytes to
shift the peer's payload in — the data path of a real board-to-board SPI link
(not cycle-accurate clock duplex). `spi-link` is a generic SSI device
(`hw/ssi/spi_link.c`), shared with the i.MX 91.

The oracle (`spilink.c`, raw spidev ioctls, static) checks the payload
**byte-exact**:

```
SPILINK:PASS: 33 bytes crossed the SPI link byte-exact
PASS: payload crossed LPSPI<->spi-link<->socket<->spi-link<->LPSPI byte-exact between two i.MX 93 guests
```

The harness enables LPSPI1 (disabled in the stock dtb), drops its `dmas` (the
transfers are small PIO), and adds a `spidev@0` slave (`rohm,dh2228fv`) so Linux
binds `/dev/spidevN.0`.

### LPSPI model work behind this

Two fixes were needed for a real `fsl-lpspi` controller to bind and move data:

- **`spi_register_controller` -EINVAL.** The driver reads num-cs from
  `PARAM[19:16]` (PCSNUM) for `fsl,imx93-spi`; PCSNUM=0 gives num_chipselect=0
  and the controller fails to register. PARAM now reports 4 PCS.
- **Frame-complete (FCF).** The driver keeps `TCR.CONT` asserted across a
  message and waits on FCF after the last byte, so the model raises `TCF|FCF`
  per frame (gating FCF on `!CONT` hangs the transfer).

### Gotchas (each cost a run)

- **`-smp 3`** (fixed 2×A55 + M33 topology), as with UART.
- **spidev/lpspi are builtin** in the BSP kernel — no module load needed (unlike
  cdc-acm for the USB-CDC link).
- **Receiver clocks in 8-byte chunks.** The LPSPI model's RX FIFO is 16 deep and
  the driver writes a whole spidev transfer to TDR before draining RX, so a
  receive larger than the FIFO overflows and drops bytes. The sender is
  unaffected (the shift always happens; only its ignored RX overflows).
- **Generous window overlap.** The receiver clocks for ~45 s and the sender
  resends across it — the spi-link FIFO buffers, but the two guests' boot
  offsets mean a short window can miss the sends entirely.

### Cross-SoC: 93 ↔ MCXN947 (`run-spi-mcx.sh`)

The same `spi-link` transport bridges *different* SoCs. `run-spi-mcx.sh` runs a
live cross-check between this i.MX 93 (a **Linux `fsl-lpspi`** master over
`/dev/spidev`) and the **MCXN947** (a **bare-metal Cortex-M33** master, the
`mcxn947qemu` `tests/mcxn-spi-link` firmware). Each attaches a `spi-link` to its
named LPSPI bus, joined by a unix socket; the 93 clocks a `[0xA5, 32-byte
pattern]` frame out (the M33 collects + verifies it) while draining the M33's
`0x5A` stream in (the 93 verifies it) — byte-exact both directions:

```
i.MX 93:  SPIPEER: MOSI-in 128 bytes, 0 not-0x5A -> RXOK
MCXN947:  SPI LINK PASS 32
PASS: i.MX 93 Linux fsl-lpspi <-> MCXN947 bare-metal M33, byte-exact both directions
```

Cross-repo: it needs a built `mcxn947qemu` QEMU + `arm-none-eabi-gcc` for the
M33 firmware (set `RMCX=` to the checkout); it `SKIP`s cleanly if absent. One
gotcha: the M33 firmware is **one-shot** (streams `0x5A`, collects 32, then
stops), so the 93 peer clocks a **bounded** byte count once — a busy loop would
fill the peer's 256-deep spi-link FIFO once the M33 stops draining, and the
resulting socket backpressure blocks the LPSPI TDR write.
