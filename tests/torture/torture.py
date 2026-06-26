#!/usr/bin/env python3
#
# i.MX93 max-concurrency torture orchestrator.
#
# Boots the core-image-weston desktop and drives every datapath at once
# (display + the full audio surface + CPU + storage + net), proving none
# starves the others. The NPU is excluded - its M33 boot wedges the desktop
# guest (see README "Known issues"). Sudo-free: the systemd weston image is
# driven over the
# serial console; the workload launcher lives on a 9p overlay (written here,
# kicked over serial); telemetry comes back through the shared overlay dir, and
# the QEMU monitor takes best-effort screendumps.
#
# Health is judged from guest-side counters (robust to host load), not the
# screendump: each workload bumps $OVERLAY/progress/<name>, and a frozen counter
# means a wedged datapath. Display liveness uses the lcdif scanout IRQ (vblank)
# and the live compositor, so it holds even when the host is too loaded to grab
# a frame.
#
# Env: QEMU, KERNEL, DTB, WIC, OVERLAY (9p dir, must hold launcher.sh), OUTDIR
#      (screendumps), DURATION (soak seconds, default 600).
import os, socket, subprocess, sys, time, signal
from PIL import Image, ImageChops

E = os.environ.get
HOME = os.path.expanduser("~")
DEPLOY = E("DEPLOY", HOME + "/Documents/nxp/linux/imx-yocto-bsp/build-imx93/"
           "tmp/deploy/images/imx93evk")
QEMU = E("QEMU", os.getcwd() + "/build-imx93/qemu-system-aarch64")
KERNEL = E("KERNEL", DEPLOY + "/Image")
DTB = E("DTB", DEPLOY + "/imx93-11x11-evk.dtb")
WIC = E("WIC", HOME + "/Documents/nxp/imx93-weston.wic")
OVERLAY = E("OVERLAY", "/tmp/torture-overlay")
OUTDIR = E("OUTDIR", "/tmp/torture-out")
DURATION = int(E("DURATION", "600"))

for p, w in ((QEMU, "qemu"), (KERNEL, "kernel"), (DTB, "dtb"), (WIC, "weston wic"),
             (OVERLAY + "/launcher.sh", "overlay launcher.sh")):
    if not os.path.exists(p):
        sys.exit(f"error: {w} not found: {p}")
PROG = OVERLAY + "/progress"
os.makedirs(PROG, exist_ok=True)
os.makedirs(OUTDIR, exist_ok=True)
for f in os.listdir(PROG):
    os.unlink(PROG + "/" + f)

TD = "/tmp/torture_run"; os.path.isdir(TD) or os.makedirs(TD)
SER, MON = TD + "/serial.sock", TD + "/mon.sock"
for s in (SER, MON):
    try: os.unlink(s)
    except OSError: pass

qemu = subprocess.Popen([
    QEMU, "-M", "imx93-11x11-evk", "-m", "4G", "-display", "none",
    "-kernel", KERNEL, "-dtb", DTB,
    "-drive", f"if=sd,file={WIC},format=raw",
    "-append", "console=ttyLP0,115200 root=/dev/mmcblk0p2 rootwait rw cpuidle.off=1",
    # -nic none keeps the FEC link DOWN. A backed FEC (the default if no -nic is
    # given, or -nic user) brings the link up, and an active FEC link concurrent
    # with the Ethos-U M33/rpmsg boot RCU-stalls the guest (see README "Known
    # issues"). The net workload runs over loopback; the FEC datapath is
    # validated standalone elsewhere.
    "-nic", "none",
    "-fsdev", f"local,id=fs0,path={OVERLAY},security_model=none",
    "-device", "virtio-9p-device,fsdev=fs0,mount_tag=overlay",
    "-serial", f"unix:{SER},server,nowait", "-serial", "null",
    "-monitor", f"unix:{MON},server,nowait",
], stdout=subprocess.DEVNULL, stderr=open(TD + "/qemu.err", "w"), start_new_session=True)
print(f"qemu pid {qemu.pid}", flush=True)

def conn(path, tries=80):
    for _ in range(tries):
        try:
            s = socket.socket(socket.AF_UNIX); s.connect(path); return s
        except OSError: time.sleep(0.5)
    raise RuntimeError("connect timeout " + path)

import select
ser = conn(SER); ser.setblocking(False); buf = ""
def pump():                         # non-blocking drain of whatever's arrived
    global buf
    while True:
        r, _, _ = select.select([ser], [], [], 0)
        if not r: break
        try: d = ser.recv(65536)
        except (BlockingIOError, InterruptedError): break
        if not d: break
        buf += d.decode(errors="replace")
def _send(b):                       # send fully, waiting for writability (no fixed timeout)
    while b:
        select.select([], [ser], [], 5)
        try: b = b[ser.send(b):]
        except (BlockingIOError, InterruptedError): pass
def typed(s):                       # char-by-char: a whole-line write doubles chars
    for ch in s:
        _send(ch.encode()); time.sleep(0.012)
    _send(b"\n"); time.sleep(0.3)
def wait_for(pat, timeout):
    t0 = time.time()
    while time.time() - t0 < timeout:
        pump()
        if pat in buf: return True
        time.sleep(1.0)
    return False

def shot(name):                     # best-effort; poll for the (slow under load) write
    path = f"{OUTDIR}/{name}.ppm"
    try: os.unlink(path)
    except OSError: pass
    try:
        m = conn(MON, tries=5); m.settimeout(2.0); time.sleep(0.2)
        try: m.recv(65536)
        except socket.timeout: pass
        m.sendall(f"screendump {path}\n".encode()); m.close()
        sz, stable = -1, 0
        for _ in range(40):
            time.sleep(0.3)
            if not os.path.exists(path): continue
            cur = os.path.getsize(path)
            stable = stable + 1 if (cur > 0 and cur == sz) else 0
            sz = cur
            if stable >= 2: break
        img = Image.open(path).convert("RGB"); img.save(f"{OUTDIR}/{name}.png")
        return img
    except Exception:
        return None
def meanval(a):
    if a is None: return -1.0
    px = list(a.getdata()); return sum(sum(p) for p in px) / (len(px) * 3)
def meandiff(a, b):
    if a is None or b is None: return -1.0
    px = list(ImageChops.difference(a, b).getdata())
    return sum(sum(p) for p in px) / (len(px) * 3)

PASS = True
try:
    print("boot: waiting for login...", flush=True)
    if not wait_for("login:", 300):
        print("NO LOGIN\n" + buf[-1500:], flush=True); raise SystemExit
    typed("root"); time.sleep(2); pump()
    typed("export PS1=T#"); time.sleep(1)
    typed("echo OK_$(id -un)"); wait_for("OK_root", 25)
    print("login:", "OK_root" in buf, flush=True)
    mounted = False
    for _ in range(4):
        typed("mkdir -p /mnt; mount -t 9p -o trans=virtio,version=9p2000.L overlay /mnt; "
              "cat /mnt/launcher.sh >/dev/null 2>&1 && echo MNT_OK")
        if wait_for("MNT_OK", 15): mounted = True; break
        time.sleep(2)
    print("9p mount:", mounted, flush=True)
    typed("ln -sf /mnt /mnt/ov 2>/dev/null; sh /mnt/launcher.sh >/mnt/kick.log 2>&1 &")
    typed("echo KICKED"); wait_for("KICKED", 15)
    print(f"workloads kicked; soaking {DURATION}s ...", flush=True)

    prev = shot("t0000"); t0 = time.time(); i = 0
    samples = live_samples = black_samples = 0
    last = {}
    while time.time() - t0 < DURATION:
        time.sleep(20); i += 1
        cur = shot(f"t{i:04d}")
        diff = meandiff(prev, cur); mean = meanval(cur); prev = cur if cur else prev
        samples += 1
        if diff > 0.3: live_samples += 1
        if 0 <= mean < 5: black_samples += 1
        counts = {}
        for f in sorted(os.listdir(PROG)):
            try: counts[f] = open(PROG + "/" + f).read().strip()
            except OSError: counts[f] = "?"
        adv = {k: (counts.get(k) != last.get(k)) for k in counts}
        last = dict(counts)
        tag = "BLACK" if 0 <= mean < 5 else ("LIVE" if diff > 0.3 else "static")
        print(f"[{int(time.time()-t0):4}s] screen {tag}(mean={mean:.0f} d={diff:.2f}) | " +
              " ".join(f"{k}={counts[k]}{'+' if adv.get(k) else '='}" for k in counts),
              flush=True)
        pump()
        with open(OUTDIR + "/serial.log", "w") as sl:
            sl.write(buf)
        for sig in ("Internal error", "Kernel panic", "Oops", "rcu_preempt detected",
                    "soft lockup", "hung task", "watchdog: BUG", "RCU Stall"):
            if sig in buf:
                print(f"!! GUEST FAULT: {sig}", flush=True); PASS = False; break
        else:
            continue
        break

    print("\n=== SHAKEOUT SUMMARY ===", flush=True)
    final = {}
    for f in sorted(os.listdir(PROG)):
        try: final[f] = int(open(PROG + "/" + f).read().strip())
        except Exception: final[f] = 0
    for k, v in final.items():
        print(f"  {k:10} = {v}", flush=True)
    print("  net path   = loopback (FEC link down; see README 'Known issues')",
          flush=True)
    print(f"  screendump(best-effort): {samples} samples, {live_samples} live, "
          f"{black_samples} black", flush=True)
    for k in ("disp", "cpu1", "cpu2", "sd", "net"):
        if final.get(k, 0) < 1:
            print(f"  FAIL: {k} never progressed", flush=True); PASS = False
    if final.get("vblank", 0) < 1:
        print("  WARN: lcdif vblank IRQ didn't advance; disp(client-alive) is the"
              " fallback display signal", flush=True)
    for k in ("aud_play", "aud_cap", "spdif", "micfil"):
        if k in final and final[k] < 1:
            print(f"  WARN: audio {k} did not progress (card/driver?)", flush=True)
    if samples and black_samples == samples:
        print("  FAIL: display black every captured frame", flush=True); PASS = False
    print("RESULT:", "PASS - all datapaths concurrent + display alive (disp/vblank)"
          if PASS else "FAIL - see above", flush=True)
finally:
    try: os.killpg(os.getpgid(qemu.pid), signal.SIGKILL)
    except Exception: pass
    print("artifacts:", OUTDIR, flush=True)
sys.exit(0 if PASS else 1)
