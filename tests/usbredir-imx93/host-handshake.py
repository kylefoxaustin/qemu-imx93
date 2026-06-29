#!/usr/bin/env python3
"""
i.MX93 USB inter-QEMU link (mission #5) — host-end usbredir handshake test.

Confirms the i.MX93 machine's stock `-device usb-redir` importer speaks the
usbredir wire: it emits a valid usbredir *hello* packet (type 0, version
string) as soon as the chardev connects. The i.MX93 side stays 100% stock
usb-redir (zero model coupling); the full link is achieved by pairing this host
end with the MCX device end over the same socket.

Two modes (the assertion is identical — 93 emits a valid hello — only the socket
roles differ):

  --mode client   (default) 93 is the usbredir CLIENT, a stand-in listener plays
                  the MCX device/exporter role. THIS MIRRORS THE REAL LAB
                  CONTRACT (holobench docs/TOPOLOGIES.md): device=listener,
                  host=client (-chardev ...,server=off,reconnect=N).
  --mode server   93 is the usbredir SERVER; a plain socket peer connects. A
                  standalone host-end wire self-check (no contract role).

Real lab invocation (paired with the actual MCX server, not this stand-in):
  qemu ... -chardev socket,id=ur0,path=<lab>/usb-<link>.sock,server=off,reconnect=2 \\
           -device usb-redir,chardev=ur0

Exit 0 on PASS. QEMU=/path/to/qemu-system-aarch64 (default ./qemu-system-aarch64).
"""
import argparse, os, re, socket, struct, subprocess, sys, time

QEMU = os.environ.get("QEMU", "./qemu-system-aarch64")


def assert_hello(data):
    """Return (ok, info) for a buffer that should start with a usbredir hello."""
    if len(data) < 8:
        return False, f"short read ({len(data)}B)"
    ptype, plen = struct.unpack_from("<II", data, 0)
    runs = [m.decode("latin1") for m in re.findall(rb"[ -~]{6,}", data)]
    ver = next((r for r in runs if "usb-redir" in r or "qemu" in r.lower()), "")
    info = f"type={ptype} (0=hello) length={plen} version={ver!r}"
    return (ptype == 0 and bool(ver)), info


def qemu_cmd(chardev):
    return [QEMU, "-machine", "imx93-11x11-evk", "-display", "none",
            "-serial", "none", "-monitor", "none",
            "-chardev", chardev, "-device", "usb-redir,chardev=ur0"]


def run_client(port):
    """93 = client (real lab contract). We listen as the device/exporter
    stand-in; QEMU's usb-redir dials out and sends its hello."""
    lsock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    lsock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    lsock.bind(("127.0.0.1", port))
    lsock.listen(1)
    lsock.settimeout(12)
    # reconnect-ms (not the deprecated reconnect= seconds form) on QEMU 11.x:
    # the client retries until our listener (the device/exporter) is up.
    cd = f"socket,id=ur0,host=127.0.0.1,port={port},server=off,reconnect-ms=500"
    p = subprocess.Popen(qemu_cmd(cd), stdout=subprocess.PIPE,
                         stderr=subprocess.STDOUT, text=True)
    try:
        conn, _ = lsock.accept()          # QEMU connected as client
        conn.settimeout(3)
        data = b""
        try:
            while len(data) < 96:
                c = conn.recv(256)
                if not c:
                    break
                data += c
        except socket.timeout:
            pass
        return data, p
    finally:
        lsock.close()
        stop(p)


def run_server(port):
    """93 = server (standalone self-check). We connect as a plain peer and
    read QEMU's hello."""
    cd = f"socket,id=ur0,host=127.0.0.1,port={port},server=on,wait=off"
    p = subprocess.Popen(qemu_cmd(cd), stdout=subprocess.PIPE,
                         stderr=subprocess.STDOUT, text=True)
    sock = None
    deadline = time.time() + 10
    while time.time() < deadline:
        if p.poll() is not None:
            print("QEMU exited early:\n", p.stdout.read())
            return b"", p
        try:
            sock = socket.create_connection(("127.0.0.1", port), timeout=1)
            break
        except OSError:
            time.sleep(0.2)
    data = b""
    if sock:
        sock.settimeout(3)
        try:
            while len(data) < 96:
                c = sock.recv(256)
                if not c:
                    break
                data += c
        except socket.timeout:
            pass
    return data, p


def stop(p):
    p.terminate()
    try:
        p.wait(timeout=5)
    except subprocess.TimeoutExpired:
        p.kill()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--mode", choices=["client", "server"], default="client")
    ap.add_argument("--port", type=int, default=int(os.environ.get("PORT", "14037")))
    args = ap.parse_args()

    role = ("93=CLIENT, stand-in device listener (real lab contract)"
            if args.mode == "client" else
            "93=SERVER, plain socket peer (standalone self-check)")
    print(f"mode: {args.mode} — {role}")
    data, p = (run_client if args.mode == "client" else run_server)(args.port)
    stop(p)

    if not data:
        print("FAIL: usb-redir host end sent nothing (silent)")
        return 4
    ok, info = assert_hello(data)
    print("hello:", info)
    print("PASS: usbredir wire live from the i.MX93 host end" if ok
          else "FAIL: not a valid usbredir hello")
    return 0 if ok else 5


if __name__ == "__main__":
    sys.exit(main())
