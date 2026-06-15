# BusyBox initramfs (userspace shakeout / switch_root helper)

A static-aarch64 BusyBox initramfs for exercising real userspace on the imx93
machine, and as the tiny pre-init that `tests/weston/run.sh` uses to patch the
rootfs (weston.ini → pixman) before `switch_root`ing into the image's systemd.

```
tests/busybox-initramfs/build.sh
INITRD=$(pwd)/tests/busybox-initramfs/busybox-initramfs.cpio.gz tests/weston/run.sh
```

`build.sh` fetches the prebuilt static aarch64 busybox from Ubuntu's arm64
`busybox-static` package (no cross-compiler needed) and packs an initramfs
whose `/init` mounts proc/sys, prints `uname` + the CPU count, and drops to a
shell. The generated `.cpio.gz` is not committed (build it locally).

The busybox binary is arch-generic (static aarch64) and machine-agnostic — the
same builder serves the i.MX93 and the i.MX91; only the `/init` banner differs.
