#!/usr/bin/env python3
"""
Migration test for the i.MX 93 uSDHC VEND_SPEC vmstate subsection.

VEND_SPEC (0xc0) resets to 0x3000_7809 on i.MX 9 and is read-modify-written by
sdhci-esdhc-imx, so it must survive migration. It is carried in an opt-in
subsection whose predicate is:

    needed  <=>  vendor_spec != vendor_spec_reset

i.e. a subsection may be omitted iff the field already holds what reset() would
put there (an absent subsection leaves the destination holding the reset value).

This asserts BOTH halves, because the values alone are not enough - a predicate
that simply returned true would round-trip every value correctly while silently
adding a subsection to the migration stream of every existing SDHCI guest:

  1. VALUE:  the guest's value survives a real migrate, including a deliberate
             zero written over the non-zero reset (treating zero as "absent" is
             the bug this predicate exists to avoid).
  2. FORMAT: the state file is SMALLER when the field is untouched - the
             subsection is genuinely omitted, not merely harmless.

Run:  QEMU=build-imx93/qemu-system-aarch64 python3 tests/usdhc-imx93/migrate.py
"""

import json
import ctypes
import os
import signal
import socket
import subprocess
import sys
import tempfile
import time

QEMU = os.environ.get("QEMU", "build-imx93/qemu-system-aarch64")

PR_SET_PDEATHSIG = 1


def die_with_parent():
    """Ask the kernel to SIGKILL this child if its parent ever dies.

    close() below already kills the guest, but it runs from a finally block -
    and a finally block dies with its interpreter. SIGKILL this script (or drop
    the terminal) and the guest is orphaned with nothing left that can stop it:
    a qtest QEMU blocked forever on a stdin that will never send another
    command. That corpse burns 0% CPU, so it never shows up in a census sorted
    by CPU, yet it still holds memory and its sockets. It is the most common way
    a process outlives its run, and the hardest to notice.

    A bound that lives in the PARENT is not a bound. Put it in the kernel: it
    cannot be skipped, and it survives a SIGKILL to us.
    """
    ctypes.CDLL("libc.so.6", use_errno=True).prctl(PR_SET_PDEATHSIG, signal.SIGKILL)

MACHINE = "imx93-11x11-evk"
USDHC1 = 0x42850000
VEND_SPEC = USDHC1 + 0xC0
VEND_SPEC_RESET = 0x30007809

#
# The i.MX 8M Plus shares TYPE_IMX_USDHC but never sets vendor-spec-reset, so it
# comes up on the property's default of 0 - and sdhci-esdhc-imx still writes the
# register at runtime (FRC_SDCLK_ON). That is the case a predicate keyed on
# "vendor_spec_reset != 0" gets wrong: it is false forever there, the subsection
# is never emitted, and the guest's value dies at the migration boundary.
#
# A test that only ever runs the machine it was written for cannot see a bug
# that lives in the default, so run the i.MX 8M Plus too. (-global cannot stand
# in for it: a board that sets the property in realize overrides -global, and
# the case would pass while testing nothing.)
#
MACHINE_DEFAULT = "imx8mp-evk"
USDHC1_8MP = 0x30B40000
VEND_SPEC_8MP = USDHC1_8MP + 0xC0
VEND_SPEC_RESET_8MP = 0x00000000


class Vm:
    """A qtest-driven QEMU with a QMP socket, for MMIO pokes + migration."""

    def __init__(self, td, tag, incoming=None, machine=MACHINE):
        self.td = td
        self.qmp_path = f"{td}/{tag}-qmp.sock"
        self.qt_path = f"{td}/{tag}-qt.sock"
        args = [
            QEMU, "-machine", machine, "-accel", "qtest", "-display", "none",
            "-audio", "driver=none",
            "-qtest", f"unix:{self.qt_path},server=on,wait=off",
            "-qmp", f"unix:{self.qmp_path},server=on,wait=off",
        ]
        if incoming:
            args += ["-incoming", incoming]
        self.p = subprocess.Popen(args, stdout=subprocess.DEVNULL,
                                  stderr=subprocess.PIPE, text=True,
                                  start_new_session=True,
                                  preexec_fn=die_with_parent)
        self._connect()

    def _connect(self):
        for _ in range(100):
            if os.path.exists(self.qmp_path) and os.path.exists(self.qt_path):
                break
            time.sleep(0.05)
        else:
            raise RuntimeError("qemu sockets never appeared")
        self.qmp = socket.socket(socket.AF_UNIX)
        self.qmp.connect(self.qmp_path)
        self.qf = self.qmp.makefile("rw")
        self.qf.readline()                      # greeting
        self.qmp_cmd({"execute": "qmp_capabilities"})
        self.qt = socket.socket(socket.AF_UNIX)
        self.qt.connect(self.qt_path)
        self.qtf = self.qt.makefile("rw")

    def qmp_cmd(self, cmd):
        self.qf.write(json.dumps(cmd) + "\n")
        self.qf.flush()
        while True:
            r = json.loads(self.qf.readline())
            if "return" in r or "error" in r:
                return r

    def qmp_wait_migration(self, want):
        for _ in range(200):
            r = self.qmp_cmd({"execute": "query-migrate"})
            st = r.get("return", {}).get("status")
            if st == want:
                return
            if st == "failed":
                raise RuntimeError(f"migration failed: {r}")
            time.sleep(0.05)
        raise RuntimeError(f"migration never reached {want}")

    def qt_cmd(self, line):
        self.qtf.write(line + "\n")
        self.qtf.flush()
        return self.qtf.readline().strip()

    def readl(self, addr):
        r = self.qt_cmd(f"readl 0x{addr:x}")
        return int(r.split()[-1], 16)

    def writel(self, addr, val):
        self.qt_cmd(f"writel 0x{addr:x} 0x{val:x}")

    def close(self):
        try:
            self.p.kill()
            self.p.wait(timeout=5)
        except Exception:
            pass


def round_trip(td, tag, write_val, machine=MACHINE, addr=VEND_SPEC,
               expect_reset=None):
    """Boot, optionally write VEND_SPEC, migrate to a file, reload, read back.

    If expect_reset is given, assert the register really came up holding it
    BEFORE writing anything: a case that does not verify its own setup is
    testing something else, and would pass while proving nothing.

    Returns (value_before, value_after, state_file_size).
    """
    state = f"{td}/{tag}.state"
    src = Vm(td, f"{tag}-src", machine=machine)
    try:
        if expect_reset is not None:
            got = src.readl(addr)
            if got != expect_reset:
                raise RuntimeError(
                    f"{tag}: setup check failed - {machine} came up with "
                    f"VEND_SPEC=0x{got:08x}, expected the reset value "
                    f"0x{expect_reset:08x}; this case is not exercising the "
                    f"path it claims to")
        if write_val is not None:
            src.writel(addr, write_val)
        before = src.readl(addr)
        src.qmp_cmd({"execute": "migrate",
                     "arguments": {"uri": f"file:{state}"}})
        src.qmp_wait_migration("completed")
    finally:
        src.close()

    dst = Vm(td, f"{tag}-dst", incoming=f"file:{state}", machine=machine)
    try:
        dst.qmp_wait_migration("completed")
        after = dst.readl(addr)
    finally:
        dst.close()

    return before, after, os.path.getsize(state)


def check_fresh():
    """Refuse to run against a binary older than the source it exercises.

    A stale binary produces a quiet, plausible, wrong number - and "my new
    assertion found nothing" is a very comfortable thing to believe. Never test
    a binary you did not just build.
    """
    src = os.path.join(os.path.dirname(__file__), "..", "..",
                       "hw", "sd", "sdhci.c")
    if not (os.path.exists(QEMU) and os.path.exists(src)):
        return
    if os.path.getmtime(src) > os.path.getmtime(QEMU):
        sys.exit(f"refusing to run: {QEMU} is OLDER than hw/sd/sdhci.c - "
                 f"rebuild it first (a stale binary fails quietly and "
                 f"plausibly)")


def main():
    check_fresh()
    failures = []
    with tempfile.TemporaryDirectory() as td:
        # (tag, value the guest writes or None, expected value after migrate)
        cases = [
            ("untouched", None, VEND_SPEC_RESET),
            ("guest-zero", 0x00000000, 0x00000000),
            ("guest-bits", 0x30007B09, 0x30007B09),
            ("frc-sdclk", 0x00000100, 0x00000100),
        ]
        sizes = {}
        for tag, wr, want in cases:
            before, after, size = round_trip(td, tag, wr)
            sizes[tag] = size
            ok = (before == want) and (after == want)
            print(f"{'ok  ' if ok else 'FAIL'} {tag:10s} "
                  f"0x{before:08x} -> 0x{after:08x}  (want 0x{want:08x})  "
                  f"state {size:,} B")
            if not ok:
                failures.append(f"{tag}: 0x{before:08x} -> 0x{after:08x}, "
                                f"want 0x{want:08x}")

        # FORMAT half: the subsection must be genuinely omitted when the field
        # still holds its reset value, and present when it does not.
        base = sizes["untouched"]
        for tag in ("guest-zero", "guest-bits", "frc-sdclk"):
            delta = sizes[tag] - base
            if delta <= 0:
                print(f"FAIL wire-format {tag}: state not larger than the "
                      f"untouched baseline (delta {delta} B) - the subsection "
                      f"was not emitted")
                failures.append(f"wire-format {tag}: delta {delta}")
            else:
                print(f"ok   wire-format {tag}: +{delta} B over the untouched "
                      f"baseline (the subsection)")

        #
        # The default-reset machine. i.MX 8M Plus shares this device model and
        # never sets vendor-spec-reset, so it comes up at 0 - yet its driver
        # writes the register. A predicate keyed on "vendor_spec_reset != 0" is
        # false forever here, never emits the subsection, and the guest's value
        # dies at the migration boundary. The i.MX 93 cases above cannot see
        # that: they only ever run a machine where the reset is non-zero.
        #
        before, after, _ = round_trip(
            td, "imx8mp-frc", 0x00000100,
            machine=MACHINE_DEFAULT, addr=VEND_SPEC_8MP,
            expect_reset=VEND_SPEC_RESET_8MP)
        ok = (before == 0x100) and (after == 0x100)
        print(f"{'ok  ' if ok else 'FAIL'} imx8mp     "
              f"0x{before:08x} -> 0x{after:08x}  (want 0x00000100)  "
              f"[reset=0 machine: the default path]")
        if not ok:
            failures.append(
                f"imx8mp (default reset=0): guest wrote 0x100, got back "
                f"0x{after:08x} - the value died at the migration boundary")

    if failures:
        print("\nFAILED:")
        for f in failures:
            print("  " + f)
        sys.exit(1)
    print("\nall migration cases passed")


if __name__ == "__main__":
    main()
