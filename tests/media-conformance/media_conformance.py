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
import os, socket, subprocess, sys, time, signal, json, select

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

MON = f"{TD}/qmp.sock"
qemu = subprocess.Popen([
    QEMU, "-M", "imx93-11x11-evk", "-m", "4G", "-display", "none",
    "-kernel", KERNEL, "-dtb", DTB, "-initrd", f"{TD}/c.cpio.gz",
    "-append", "console=ttyLP0,115200 cpuidle.off=1 rdinit=/myinit ignore_loglevel",
    "-serial", f"file:{CON}", "-serial", "null",
    "-qmp", f"unix:{MON},server,nowait",
], stdout=subprocess.DEVNULL, stderr=open(f"{TD}/qemu.err", "w"),
   start_new_session=True)
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
        mean = sum(data) / len(data) if data else 0
        return w, h, mean
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
                    if r and r[2] >= 5:
                        host_mark(f"MEDIA:PASS:{tag}-pixels:{r[0]}x{r[1]} mean={r[2]:.0f}")
                    elif r:
                        host_mark(f"MEDIA:FAIL:{tag}-pixels:{r[0]}x{r[1]} mean={r[2]:.0f} (black)")
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
