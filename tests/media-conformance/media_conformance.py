#!/usr/bin/env python3
#
# i.MX93 media-conformance host orchestrator.
#
# Boots one media DTB (display: rm67199 / camera: mt9m114) with a guest battery
# that drives the subsystem through a real upstream conformance tool (modetest /
# v4l2-compliance) and emits "MEDIA:PASS/FAIL/SKIP/SHOOT/BATTERY:DONE" markers on
# the serial console. The host tallies those into a scoreboard; for display it
# also takes a QMP screendump on MEDIA:SHOOT and scores the framebuffer non-black
# (the visual oracle the marker stream can't carry).
#
# Env: QEMU KERNEL DTB BASE_INITRD OVERLAY_DIR (files cpio'd in, incl battery.sh
#      + tool binaries) LANE (display|camera) OUTDIR DURATION.
import ctypes, os, socket, subprocess, sys, time, signal, json, select


def frame_verdict(w, h, data, bpp=3):
    """Structural display oracle.

    The old check was `mean(pixels) >= 5` - "non-black". That mean is invariant
    under any pixel permutation (an R/B channel swap, a torn/shifted pattern, a
    shuffled framebuffer all preserve the byte-sum) AND it passes a solid fill
    or a monochrome frame - the exact garbage a wrong scanout format / base
    address produces. So "the pixels are lit" is not "the right pixels are lit".

    This adds two structure metrics on a downsampled grid: brightness must VARY
    across the frame (a uniform / solid-fill frame has ~0 variance) and the
    frame must carry several distinct colours (a monochrome frame has 1). It
    still cannot catch a structure-preserving R/B swap - that needs a golden
    SMPTE signature - so that residual is documented, not hidden.

    Returns (ok, reason, metrics).
    """
    n = len(data)
    if n < bpp or w < 8 or h < 8:
        return False, "no-frame", {}
    step = max(1, n // 30000)                        # ~30k samples for the mean
    if step % bpp == 0:
        step += 1                                    # coprime with bpp: see all channels
    smp = data[::step]
    mean = sum(smp) / len(smp)
    if mean < 5:
        return False, "black", {"mean": round(mean, 1)}
    G = 12
    cells = []
    for gy in range(G):
        y = min(h - 1, int((gy + 0.5) / G * h))
        for gx in range(G):
            x = min(w - 1, int((gx + 0.5) / G * w))
            i = (y * w + x) * bpp
            cells.append((data[i], data[i + 1], data[i + 2]))
    bright = [r + g + b for (r, g, b) in cells]
    bmean = sum(bright) / len(bright)
    bvar = sum((v - bmean) ** 2 for v in bright) / len(bright)
    colours = {(r >> 5, g >> 5, b >> 5) for (r, g, b) in cells}
    met = {"mean": round(mean, 1), "var": round(bvar), "colours": len(colours)}
    if bvar < 400:
        return False, "uniform", met           # solid fill: lit but structureless
    if len(colours) < 4:
        return False, "monochrome", met         # one hue: lit but not a pattern
    return True, "ok", met


def _selftest():
    """Prove the oracle discriminates where the old mean check could not:
    a solid-fill and a monochrome frame both pass mean>=5 but must fail here."""
    def synth(w, h, kind):
        d = bytearray(w * h * 3)
        if kind == "solid":
            for i in range(len(d)):
                d[i] = 128
        elif kind == "mono":
            for p in range(w * h):
                d[p * 3 + 1] = 180              # green channel only
        elif kind == "bars":                     # 7 vertical colour bars
            bars = [(192, 192, 192), (192, 192, 0), (0, 192, 192), (0, 192, 0),
                    (192, 0, 192), (192, 0, 0), (0, 0, 192)]
            for y in range(h):
                for x in range(w):
                    r, g, b = bars[min(6, x * 7 // w)]
                    i = (y * w + x) * 3
                    d[i], d[i + 1], d[i + 2] = r, g, b
        return bytes(d)

    w, h = 96, 54
    cases = {"black": synth(w, h, "black"), "solid": synth(w, h, "solid"),
             "mono": synth(w, h, "mono"), "bars": synth(w, h, "bars")}
    old = lambda d: (sum(d[::37]) / len(d[::37])) >= 5          # the old oracle
    exp_new = {"black": False, "solid": False, "mono": False, "bars": True}
    exp_old = {"black": False, "solid": True, "mono": True, "bars": True}
    ok = True
    for k, d in cases.items():
        new_ok, reason, met = frame_verdict(w, h, d)
        old_ok = old(d)
        tag = "PASS" if (new_ok == exp_new[k] and old_ok == exp_old[k]) else "FAIL"
        if tag == "FAIL":
            ok = False
        print(f"  {tag}  {k:6s}: new={new_ok}({reason}) old={old_ok}  {met}")
    print("  --- solid + mono are the catch: old oracle PASSES them, new FAILS ---"
          if ok else "  --- SELFTEST FAILED ---")
    return 0 if ok else 1


if __name__ == "__main__" and "--selftest" in sys.argv:
    sys.exit(_selftest())

E = os.environ.get
QEMU = E("QEMU"); KERNEL = E("KERNEL"); DTB = E("DTB")
BASE = E("BASE_INITRD"); OVL = E("OVERLAY_DIR")
LANE = E("LANE", "display"); OUTDIR = E("OUTDIR", "/tmp/media-out")
DURATION = int(E("DURATION", "240"))
for n, p in (("qemu", QEMU), ("kernel", KERNEL), ("dtb", DTB), ("initrd", BASE),
             ("overlay", OVL), ("battery", OVL + "/battery.sh")):
    if not p or not os.path.exists(p):
        sys.exit(f"error: {n} not found: {p}")
os.makedirs(OUTDIR, exist_ok=True)
CON = f"{OUTDIR}/{LANE}-console.log"
MARKS = f"{OUTDIR}/{LANE}-markers.txt"            # combined guest + host markers
open(MARKS, "w").close()

TD = subprocess.run(["mktemp", "-d"], capture_output=True, text=True).stdout.strip()
# myinit: tools live at / -> add to PATH; the guest rootfs already carries
# libstdc++/libm/libgcc_s/libc, so nothing else to stage.
open(f"{TD}/myinit", "w").write(
    "#!/bin/sh\n"
    "PATH=/:/sbin:/usr/sbin:/bin:/usr/bin; export PATH\n"
    "mount -t proc proc /proc; mount -t sysfs sysfs /sys; mount -t devtmpfs devtmpfs /dev\n"
    "sleep 3\n"
    "sh /battery.sh\n"
    "while true; do sleep 5; done\n")
os.chmod(f"{TD}/myinit", 0o755)
# overlay cpio: myinit + everything in OVERLAY_DIR (battery.sh, tool binaries)
names = ["myinit"] + os.listdir(OVL)
for f in os.listdir(OVL):
    subprocess.run(["cp", "-a", f"{OVL}/{f}", f"{TD}/{f}"])
listing = "\n".join(names) + "\n"
with open(f"{TD}/o.cpio", "wb") as oc:
    subprocess.run(["cpio", "-o", "-H", "newc"], cwd=TD, input=listing.encode(),
                   stdout=oc, stderr=subprocess.DEVNULL)
with open(f"{TD}/c.cpio.gz", "wb") as cc:
    for src in (BASE, f"{TD}/o.cpio"):
        cc.write(open(src, "rb").read())

PR_SET_PDEATHSIG = 1


def die_with_parent():
    """Ask the kernel to SIGKILL this guest if this harness ever dies.

    The finally block below already killpg's the guest, but a finally block dies
    with its interpreter: SIGKILL this script, or drop the terminal, and the
    guest is orphaned with nothing left that can stop it. A bound that lives in
    the PARENT is not a bound. This one lives in the kernel, so it cannot be
    skipped and it survives a SIGKILL to us.
    """
    ctypes.CDLL("libc.so.6", use_errno=True).prctl(PR_SET_PDEATHSIG, signal.SIGKILL)

MON = f"{TD}/qmp.sock"
qemu = subprocess.Popen([
    QEMU, "-M", "imx93-11x11-evk", "-m", "4G", "-display", "none", "-audio", "driver=none",
    "-kernel", KERNEL, "-dtb", DTB, "-initrd", f"{TD}/c.cpio.gz",
    "-append", "console=ttyLP0,115200 cpuidle.off=1 rdinit=/myinit ignore_loglevel",
    "-serial", f"file:{CON}", "-serial", "null",
    "-qmp", f"unix:{MON},server,nowait",
], stdout=subprocess.DEVNULL, stderr=open(f"{TD}/qemu.err", "w"),
   start_new_session=True, preexec_fn=die_with_parent)
print(f"[{LANE}] qemu pid {qemu.pid}; console -> {CON}", flush=True)

def host_mark(line):
    with open(MARKS, "a") as f:
        f.write(line + "\n")
    print("  " + line, flush=True)

def screendump(tag):
    """QMP screendump -> PPM; return (w, h, mean) or None."""
    path = f"{OUTDIR}/{LANE}-{tag}.ppm"
    try:
        s = socket.socket(socket.AF_UNIX); s.settimeout(5); s.connect(MON)
        s.recv(4096)                                   # greeting
        s.sendall(b'{"execute":"qmp_capabilities"}\n'); s.recv(4096)
        try: os.unlink(path)
        except OSError: pass
        s.sendall(json.dumps({"execute": "screendump",
                              "arguments": {"filename": path}}).encode() + b"\n")
        s.recv(4096); s.close()
        for _ in range(40):
            time.sleep(0.3)
            if os.path.exists(path) and os.path.getsize(path) > 64:
                break
        with open(path, "rb") as f:
            magic = f.readline().strip()
            dims = f.readline().split()
            f.readline()
            data = f.read()
        w, h = int(dims[0]), int(dims[1])
        ok, reason, met = frame_verdict(w, h, data)
        return w, h, ok, reason, met
    except Exception as e:
        print(f"  screendump fail: {e}", flush=True)
        return None

# Tail the console, react to SHOOT, finish on BATTERY:DONE.
seen = 0
t0 = time.time()
done = False
shot_for = set()
while time.time() - t0 < DURATION:
    time.sleep(1)
    try:
        lines = open(CON, errors="replace").read().splitlines()
    except OSError:
        lines = []
    for ln in lines[seen:]:
        if ln.startswith("MEDIA:"):
            if ln.startswith("MEDIA:SHOOT:"):
                tag = ln.split(":", 2)[2].strip()
                if tag not in shot_for:
                    shot_for.add(tag)
                    r = screendump(tag)
                    if r and r[2]:
                        host_mark(f"MEDIA:PASS:{tag}-pixels:{r[0]}x{r[1]} {r[4]}")
                    elif r:
                        host_mark(f"MEDIA:FAIL:{tag}-pixels:{r[0]}x{r[1]} "
                                  f"{r[3]} {r[4]}")
                    else:
                        host_mark(f"MEDIA:FAIL:{tag}-pixels:no screendump")
            elif ln.startswith("MEDIA:BATTERY:DONE"):
                host_mark(ln); done = True
            else:
                host_mark(ln)
    seen = len(lines)
    if done:
        break

if not done:
    print(f"[{LANE}] TIMEOUT after {DURATION}s (no BATTERY:DONE)", flush=True)
    host_mark(f"MEDIA:FAIL:{LANE}-battery:timeout (boot/hang)")
try:
    os.killpg(os.getpgid(qemu.pid), signal.SIGKILL)
except Exception:
    pass
print(f"[{LANE}] done; markers -> {MARKS}", flush=True)
sys.exit(0)
