#!/bin/sh
# i.MX93 USB inter-QEMU link (mission #5) — usbredir host-end handshake.
# Default mode=client mirrors the real lab contract (93 is the usbredir client).
# Usage: QEMU=/path/to/qemu-system-aarch64 ./run.sh [--mode client|server]
set -e
DIR=$(dirname "$0")
: "${QEMU:=./qemu-system-aarch64}"
export QEMU
exec python3 "$DIR/host-handshake.py" "$@"
