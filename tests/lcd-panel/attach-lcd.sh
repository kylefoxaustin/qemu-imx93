#!/usr/bin/env bash
#
# Attach a reference LCD panel to an i.MX93 11x11 EVK device tree.
#
# SPDX-License-Identifier: GPL-2.0-or-later
#
# The QEMU imx93-11x11-evk machine is FAITHFUL to the real board: the stock
# imx93-11x11-evk dtb drives the LCDIFv3 display controller but attaches no
# panel (exactly like a bare EVK with nothing plugged into the DSI/LVDS
# connector), so DRM registers no connector and there is no /dev/fb0. To light
# up an LCD you do what you'd do on real silicon - attach a panel - which is a
# device-tree change, not a machine change.
#
# Unlike the i.MX95 (whose panel chain must be spliced into the dts by hand),
# the i.MX93 BSP already SHIPS ready-made panel-attached device trees - the
# board's reference panels are separately-orderable add-ons and the BSP carries
# a dtb per panel. So "attach LCD" on the 93 is simply selecting the panel dtb
# the kernel should boot; no dtc / dts surgery is required. This script resolves
# the chosen panel's prebuilt dtb and copies it to <out.dtb>, ready to pass to
# `qemu-system-aarch64 -M imx93-11x11-evk ... -dtb <out.dtb>`.
#
# Validated panels (LCDIFv3 scanout -> /dev/fb0, QMP screendump non-black):
#   rm67199  (default)  MIPI-DSI, LCDIFv3 -> dw-mipi-dsi -> Raydium RM67199,
#                       native 1080x1920  (tests/display-imx93/run.sh proves it)
#   boe-lvds            LVDS, LCDIFv3 -> LDB -> BOE WXGA panel, 1280x800
#
# Usage:   attach-lcd.sh <out.dtb> [panel]
#   DEPLOY=/path/to/bsp/deploy/images/imx93evk   (where the panel dtbs live)
set -euo pipefail

OUT=${1:?usage: attach-lcd.sh <out.dtb> [rm67199|boe-lvds]}
PANEL=${2:-rm67199}
DEPLOY=${DEPLOY:-$HOME/Documents/nxp/linux/imx-yocto-bsp/build-imx93/tmp/deploy/images/imx93evk}

case "$PANEL" in
    rm67199)  SRC="imx93-11x11-evk-rm67199.dtb";          DESC="MIPI-DSI Raydium RM67199, 1080x1920" ;;
    boe-lvds) SRC="imx93-11x11-evk-boe-wxga-lvds-panel.dtb"; DESC="LVDS BOE WXGA, 1280x800" ;;
    *) echo "error: unknown panel '$PANEL' (choose: rm67199, boe-lvds)" >&2; exit 2 ;;
esac

[ -e "$DEPLOY/$SRC" ] || {
    echo "error: panel dtb not found: $DEPLOY/$SRC" >&2
    echo "       set DEPLOY=<bsp deploy images dir> (or build the BSP)." >&2
    exit 1
}

cp -f "$DEPLOY/$SRC" "$OUT"
echo "wrote $OUT  (panel: $PANEL - $DESC)"
