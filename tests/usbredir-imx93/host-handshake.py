#!/usr/bin/env python3
"""
i.MX93 USB inter-QEMU link (mission #5, milestone M1) — host-end handshake test.

Confirms that the i.MX93 machine's stock `-device usb-redir` importer speaks the
usbredir wire: it is brought up with a usbredir socket-server chardev, a plain
socket peer connects, and we assert QEMU emits a valid usbredir *hello* packet
(type 0, version string). This is the "wire proven live" bar — the i.MX93 side
stays 100% stock usb-redir (zero model coupling), so the full link is achieved
by pairing this host end with the MCX device end over the same socket.

Exit 0 on PASS. Set QEMU=/path/to/qemu-system-aarch64 (default: ./qemu-system-aarch64).
"""
import os, re, socket, struct, subprocess, sys, time

QEMU = os.environ.get("QEMU", "./qemu-system-aarch64")
PORT = int(os.environ.get("PORT", "14037"))


def main():
    cmd = [
        QEMU, "-machine", "imx93-11x11-evk", "-display", "none",
        "-serial", "none", "-monitor", "none",
        "-chardev",
        f"socket,id=ur0,host=127.0.0.1,port={PORT},server=on,wait=off",
        "-device", "usb-redir,chardev=ur0",
    ]
    p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                         text=True)
    try:
        sock = None
        deadline = time.time() + 10
        while time.time() < deadline:
            if p.poll() is not None:
                print("QEMU exited early:\n", p.stdout.read())
                return 2
            try:
                sock = socket.create_connection(("127.0.0.1", PORT), timeout=1)
                break
            except OSError:
                time.sleep(0.2)
        if sock is None:
            print("could not connect to the usbredir socket")
            return 3

        sock.settimeout(3)
        data = b""
        try:
            while len(data) < 96:
                chunk = sock.recv(256)
                if not chunk:
                    break
                data += chunk
        except socket.timeout:
            pass

        if not data:
            print("FAIL: usb-redir host end sent nothing (silent)")
            return 4
        ptype, plen = struct.unpack_from("<II", data, 0)
        runs = [m.decode("latin1") for m in re.findall(rb"[ -~]{6,}", data)]
        ver = next((r for r in runs if "usb-redir" in r or "qemu" in r.lower()),
                   "")
        print(f"hello: type={ptype} (0=hello) length={plen} version={ver!r}")
        if ptype == 0 and ver:
            print("PASS: usbredir wire live from the i.MX93 host end")
            return 0
        print("FAIL: not a valid usbredir hello")
        return 5
    finally:
        p.terminate()
        try:
            p.wait(timeout=5)
        except subprocess.TimeoutExpired:
            p.kill()


if __name__ == "__main__":
    sys.exit(main())
