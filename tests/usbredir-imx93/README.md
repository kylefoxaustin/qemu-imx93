# i.MX93 ↔ MCX USB link (mission #5)

Holobench wires the emulator fleet together so data flows between QEMUs over
real transports. For USB the operator set **i.MX93 = USB host** and
**MCX = USB device**, joined over a `usbredir` socket. The i.MX93 side stays
**100% stock `-device usb-redir`** — zero model coupling, per Holobench's Prime
Directive.

## Socket contract (per holobench docs/TOPOLOGIES.md)

usbredir convention: the **exporter (device) listens**, the **importer (host)
connects as client**. So:

| End | Role | Chardev |
|-----|------|---------|
| **MCX** (device) | socket **SERVER / listener** | `server=on` |
| **i.MX93** (host) | socket **CLIENT** | `-chardev socket,...,server=off,reconnect-ms=2000` → stock `-device usb-redir,chardev=...` |

Real-lab invocation (paired with the actual MCX server, per-link unix socket
owned by the LabCoordinator):

```
qemu-system-aarch64 -machine imx93-11x11-evk ... \
  -chardev socket,id=ur0,path=<lab-run>/usb-<link>.sock,server=off,reconnect-ms=2000 \
  -device usb-redir,chardev=ur0
```

> Note: on QEMU 11.x use `reconnect-ms=` (the `reconnect=` seconds form is
> deprecated and silently ignored — the client then never retries).

## What this tests

`host-handshake.py` (via `run.sh`) is **M1**: it proves the i.MX93 host end
speaks the usbredir wire — QEMU's stock `usb-redir` emits a valid usbredir
**hello** (type 0, `qemu usb-redir guest <ver>`).

```
QEMU=/path/to/build/qemu-system-aarch64 ./run.sh            # client mode (contract)
QEMU=/path/to/build/qemu-system-aarch64 ./run.sh --mode server   # standalone self-check
```

- **client mode** (default) mirrors the real contract: 93 dials out as the
  usbredir client; a stand-in listener plays MCX's device/exporter role.
- **server mode** is a standalone host-end wire self-check (93 listens, a plain
  peer connects). Same assertion, swapped roles.

## Status

- **M1 host-end wire: PASS** — hello live in both modes (93 as client *and* as
  server), stock `usb-redir`.
- **Real link (M4): pending the live pairing** — point this client at MCX's
  server socket (`/tmp/holo-usb-imx93-mcx.sock` for the 2-party bring-up, or the
  coordinator's `<lab-run>/usb-<link>.sock`) and let 93's Linux USB host
  enumerate MCX's gadget. First gadget = **vendor stub** (prove enumeration),
  then **CDC-ACM** so Linux binds `/dev/ttyACM0` for real bytes both ways. Ping
  holobench when a device enumerates end-to-end through 93 to register the lab.
