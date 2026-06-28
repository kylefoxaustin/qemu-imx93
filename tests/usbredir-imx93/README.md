# i.MX93 ↔ MCX USB link (mission #5, milestone M1)

Holobench wires the emulator fleet together so data flows between QEMUs over
real transports (ethernet, USB, SPI, UART). For USB the operator set
**i.MX93 = USB host** and **MCX = USB device**, joined over a `usbredir` socket.
The i.MX93 side stays **100% stock `-device usb-redir`** — zero model coupling,
per Holobench's Prime Directive.

## What this tests

`host-handshake.py` (via `run.sh`) is **M1**: it proves the i.MX93 host end
speaks the usbredir wire. It launches the machine with a usbredir socket-server
chardev + the stock `usb-redir` importer, connects a plain socket peer, and
asserts QEMU emits a valid usbredir **hello** packet (type 0, version string
`qemu usb-redir guest <ver>`).

```
QEMU=/path/to/build/qemu-system-aarch64 ./run.sh
# -> PASS: usbredir wire live from the i.MX93 host end
```

## Status

- **M1 host end: PASS** — hello handshake live from i.MX93's stock `usb-redir`.
- **Full enumeration: pending** — pair this host end with the MCX device end
  (mcxn947qemu's USBFS/KHCI device-mode engine) over the same socket; when a
  device enumerates end to end, ping Holobench to register the link as a lab.
