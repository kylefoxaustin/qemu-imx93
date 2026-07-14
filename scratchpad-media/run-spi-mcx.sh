#!/bin/sh
# i.MX 93 <-> MCXN947 cross-SoC SPI board-to-board (shared spi_link.c transport).
# MCX end = bare-metal M33 (spilink.elf) SSI master, server; 93 end = Linux
# fsl-lpspi master over /dev/spidev, client. Both attach spi-link to their bus,
# joined by a unix socket. Verifies byte-exact both directions.
set -u
SD=/tmp/claude-1000/-home-kyle-Documents-GitHub-93emulator/04f1158d-b15a-46d2-891d-6da37089fc37/scratchpad
R93=/home/kyle/Documents/GitHub/93emulator
RMCX=/home/kyle/Documents/GitHub/mcxn947qemu
QEMU93=$R93/build-imx93/qemu-system-aarch64
QEMUMCX=$RMCX/build/qemu-system-arm
D=$HOME/Documents/nxp/linux/imx-yocto-bsp/build-imx93/tmp/deploy/images/imx93evk
DTC=$HOME/Documents/nxp/linux/imx-yocto-bsp/build-imx93/tmp/work-shared/imx93evk/kernel-build-artifacts/scripts/dtc/dtc
INITRD_SRC=$R93/tests/busybox-initramfs/busybox-initramfs.cpio.gz
FW=/tmp/mcx-spilink.elf
SOCK=/tmp/spi-mcx93.sock
MCXCON=$SD/mcx-spi-console.log
CLOG=$SD/spi93-client.log

pkill -9 -f "spi-mcx93.sock" 2>/dev/null || true; sleep 1; rm -f "$SOCK" "$MCXCON" "$CLOG"
WORK=$(mktemp -d)

# 93 client tool + dtb (enable lpspi1 + spidev@0, drop dmas)
aarch64-linux-gnu-gcc -O2 -static -o "$WORK/spi_peer" "$SD/spi_peer.c" || { echo "peer build failed"; exit 1; }
"$DTC" -I dtb -O dts "$D/imx93-11x11-evk.dtb" 2>/dev/null > "$WORK/base.dts"
awk '
  /spi@44360000 \{/ { inn = 1 }
  inn && /dmas =|dma-names =/ { next }
  inn && /status = "disabled"/ { print "\t\t\t\tstatus = \"okay\";"; next }
  inn && /^\t\t\t\};/ { print "\t\t\t\tspidev@0 {"; print "\t\t\t\t\tcompatible = \"rohm,dh2228fv\";"; print "\t\t\t\t\treg = <0x00>;"; print "\t\t\t\t\tspi-max-frequency = <0xf4240>;"; print "\t\t\t\t};"; print; inn = 0; next }
  { print }
' "$WORK/base.dts" > "$WORK/spi.dts"
"$DTC" -I dts -O dtb "$WORK/spi.dts" 2>/dev/null > "$WORK/spi.dtb"

# 93 initrd
S="$WORK/stage"; mkdir -p "$S"
zcat "$INITRD_SRC" | (cd "$S" && cpio -idmu 2>/dev/null)
install -m755 "$WORK/spi_peer" "$S/spi_peer"
cat > "$S/init" <<'INIT'
#!/bin/busybox sh
/bin/busybox mount -t proc proc /proc
/bin/busybox mount -t devtmpfs dev /dev 2>/dev/null
/bin/busybox mount -t sysfs sys /sys 2>/dev/null
/bin/busybox --install -s /bin 2>/dev/null
exec > /dev/console 2>&1
sleep 1
SP=$(ls /dev/spidev* 2>/dev/null | head -1)
[ -c "$SP" ] || { echo "SPIPEER:FAIL:no /dev/spidev"; busybox poweroff -f; }
echo "=== 93<->MCX SPI ($SP) ==="
/spi_peer "$SP"
echo "=== DONE ==="
/bin/busybox poweroff -f
INIT
chmod +x "$S/init"
( cd "$S" && find . | cpio -o -H newc 2>/dev/null | gzip ) > "$WORK/peer.gz"

# launch MCX SPI node (server)
echo "== launching MCX SPI node (server) =="
"$QEMUMCX" -M frdm-mcxn947 -display none -monitor none -serial "file:$MCXCON" \
    -chardev "socket,id=spil,path=$SOCK,server=on,wait=off" \
    -device spi-link,bus=mcxn-lpspi,chardev=spil \
    -kernel "$FW" -no-reboot >/dev/null 2>&1 &
MPID=$!
for i in $(seq 1 20); do ss -xl 2>/dev/null | grep -q spi-mcx93 && break; sleep 0.5; done

# boot 93 client
echo "== booting i.MX 93 client (spidev master) =="
timeout 120 "$QEMU93" -M imx93-11x11-evk -audio driver=none -smp 3 -m 2G -display none \
    -kernel "$D/Image" -dtb "$WORK/spi.dtb" -initrd "$WORK/peer.gz" \
    -append "console=ttyLP0,115200 cpuidle.off=1 rdinit=/init" \
    -chardev "socket,id=spil,path=$SOCK,server=off,reconnect-ms=1000" \
    -device spi-link,bus=lpspi1,chardev=spil \
    -serial "file:$CLOG" -serial null >/dev/null 2>&1
kill $MPID 2>/dev/null; rm -f "$SOCK"; rm -rf "$WORK"

echo "================== 93<->MCX SPI =================="
echo "--- 93 (Linux spidev) side ---"; grep -aE "SPIPEER" "$CLOG" | grep -avE '^\[' | tail -3
echo "--- MCX (bare-metal M33) side ---"; grep -aiE "SPI LINK|PASS" "$MCXCON" | tail -3
if grep -aq "SPIPEER:RXOK" "$CLOG" && grep -aiq "SPI LINK PASS" "$MCXCON"; then
    echo "PASS: 93 Linux fsl-lpspi <-> MCX bare-metal M33, byte-exact both directions over shared spi_link.c"
else
    echo "FAIL: cross-check incomplete"
fi
