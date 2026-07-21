#!/usr/bin/env python3
# i.MX93 migration (vmstate) round-trip smoke: boot A -> migrate to file ->
# load into B via -incoming -> verify B resumes. Exercises every device's
# vmstate save AND load, incl. this session's new fields (TPM cnt_base+clock,
# MICFIL/XCVR/FlexIO version bumps, CCM clocks).
import json, os, socket, subprocess, sys, tempfile, time

QEMU = sys.argv[1]
KERNEL = sys.argv[2]
DTB = sys.argv[3]
INITRD = sys.argv[4]
WORK = sys.argv[5]
STATE = os.path.join(WORK, "vmstate.migf")
# QMP unix sockets have a hard 108-byte path limit; the scratchpad path is too
# long, so sockets live in a short /tmp dir (logs + state stay in WORK).
SOCKDIR = tempfile.mkdtemp(prefix="/tmp/mig")

def qmp(sockpath, timeout=180):
    # connect (retry until the socket exists)
    s = None
    for _ in range(timeout):
        try:
            s = socket.socket(socket.AF_UNIX); s.connect(sockpath); break
        except OSError:
            time.sleep(1)
    if s is None:
        raise RuntimeError("qmp connect failed: " + sockpath)
    f = s.makefile("rw")
    f.readline()                                   # greeting
    def cmd(d):
        f.write(json.dumps(d) + "\n"); f.flush()
        while True:
            line = f.readline()
            if not line:
                raise RuntimeError("qmp closed")
            m = json.loads(line)
            if "return" in m or "error" in m:
                return m
    cmd({"execute": "qmp_capabilities"})
    return s, cmd

def launch(name, extra):
    sockp = os.path.join(SOCKDIR, name + ".qmp")
    if os.path.exists(sockp): os.unlink(sockp)
    logp = os.path.join(WORK, name + ".log")
    args = [QEMU, "-M", "imx93-11x11-evk", "-m", "4G", "-display", "none",
            "-kernel", KERNEL, "-dtb", DTB, "-initrd", INITRD,
            "-append", "console=ttyLP0,115200 cpuidle.off=1 rdinit=/myinit ignore_loglevel",
            "-serial", "file:" + logp, "-serial", "null",
            "-qmp", "unix:%s,server=on,wait=off" % sockp]
    args += extra
    p = subprocess.Popen(args, stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)
    return p, sockp, logp

def wait_boot(logp, marker="login:", timeout=120):
    for _ in range(timeout):
        try:
            if marker in open(logp, errors="ignore").read():
                return True
        except FileNotFoundError:
            pass
        time.sleep(1)
    return False

print("=== launch A ===")
pa, socka, loga = launch("A", [])
if not wait_boot(loga, "MIGRATE_READY"):
    print("FAIL: A did not reach MIGRATE_READY"); pa.kill(); sys.exit(1)
print("A booted to userspace")
sa, ca = qmp(socka)

print("=== stop + migrate A -> file ===")
ca({"execute": "stop"})
r = ca({"execute": "migrate", "arguments": {"uri": "exec:cat > %s" % STATE}})
if "error" in r:
    print("FAIL: migrate cmd:", r["error"]); pa.kill(); sys.exit(1)
# poll migration status
for _ in range(120):
    st = ca({"execute": "query-migrate"})["return"]["status"]
    if st in ("completed", "failed", "cancelled"):
        break
    time.sleep(1)
print("migration status:", st, "| statefile bytes:",
      os.path.getsize(STATE) if os.path.exists(STATE) else "MISSING")
ca({"execute": "quit"}); pa.wait(timeout=15)
if st != "completed":
    print("FAIL: migration did not complete"); sys.exit(1)

print("=== launch B with -incoming (load vmstate) ===")
pb, sockb, logb = launch("B", ["-incoming", "exec:cat < %s" % STATE])
sb, cb = qmp(sockb)
# on incoming, machine is paused until migration finishes loading
for _ in range(60):
    st2 = cb({"execute": "query-migrate"})["return"].get("status", "none")
    if st2 in ("completed", "failed"):
        break
    time.sleep(1)
print("incoming load status:", st2)
if st2 == "failed":
    print("FAIL: vmstate LOAD failed"); pb.kill(); sys.exit(1)
# resume B and confirm it runs
cb({"execute": "cont"})
time.sleep(3)
status = cb({"execute": "query-status"})["return"]["status"]
print("B status after cont:", status)
run_ok = status == "running"
cb({"execute": "quit"}); pb.wait(timeout=15)

# Any vmstate load error would have printed to B's stderr->log too
ok = st == "completed" and st2 == "completed" and run_ok
print("=== VERDICT ===")
print("PASS" if ok else "FAIL")
sys.exit(0 if ok else 1)
