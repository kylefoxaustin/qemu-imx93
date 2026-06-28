#!/bin/sh
# i.MX93 USB inter-QEMU link (mission #5) — M1 host-end handshake.
# Usage: QEMU=/path/to/qemu-system-aarch64 ./run.sh
set -e
DIR=$(dirname "$0")
: "${QEMU:=./qemu-system-aarch64}"
export QEMU
exec python3 "$DIR/host-handshake.py"
