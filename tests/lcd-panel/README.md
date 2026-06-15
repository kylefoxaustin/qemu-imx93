# Attaching an LCD panel to the i.MX93 11x11 EVK

The `imx93-11x11-evk` QEMU machine is **faithful to the real board**: the stock
`imx93-11x11-evk` device tree drives the LCDIFv3 display controller but attaches
**no panel** — exactly like a bare EVK with nothing plugged into the DSI/LVDS
connector — so DRM registers no connector and there is no `/dev/fb*`. That is
correct, not a bug: the EVK's reference panels (a MIPI-DSI Raydium RM67199, a BOE
WXGA LVDS panel, …) are **separately-orderable add-ons**, and on real hardware
you select the matching device tree for the panel you plugged in.

## The 93 vs the 95: no dts surgery needed

On the i.MX95, the stock dts ships the panel chain **disabled**, so its
`attach-lcd.sh` has to splice the DPU → LDB → LVDS path and a `panel-lvds` node
into the dts by hand (with `dtc`). The i.MX93 BSP instead **ships ready-made
panel-attached device trees** — one dtb per reference panel — so "attach LCD"
here is simply **selecting the panel dtb** the kernel boots. No `dtc`, no dts
splice. The machine itself is **not** modified (it stays faithful); attaching a
panel is purely a device-tree choice.

`attach-lcd.sh` resolves the chosen panel's prebuilt dtb and copies it to an
output path you pass to `-dtb`:

```
./attach-lcd.sh imx93-11x11-evk-lcd.dtb            # default: rm67199 (DSI)
./attach-lcd.sh imx93-11x11-evk-lcd.dtb boe-lvds   # LVDS panel instead
qemu-system-aarch64 -M imx93-11x11-evk ... -dtb imx93-11x11-evk-lcd.dtb
```

`DEPLOY=<bsp deploy images dir>` overrides where the panel dtbs are found
(default `~/Documents/nxp/linux/imx-yocto-bsp/build-imx93/tmp/deploy/images/imx93evk`).

## Panels

| name (arg)        | chain                                   | resolution |
|-------------------|-----------------------------------------|------------|
| `rm67199` (default) | LCDIFv3 → dw-mipi-dsi → Raydium RM67199 | 1080×1920  |
| `boe-lvds`        | LCDIFv3 → LDB → BOE WXGA LVDS panel      | 1280×800   |

The RM67199 DSI path is the validated one: `tests/display-imx93/run.sh` fills
`/dev/fb0` and QMP-screendumps a non-black **1080×1920** frame, proving the
LCDIFv3 scans the framebuffer out through the DSI panel.

## "Attach LCD" toggle in a UI

For an "Attach LCD" button (e.g. holobench), generate the `-lcd.dtb` once with
`attach-lcd.sh` and have the control swap `-dtb` between the stock
`imx93-11x11-evk.dtb` (no panel → no `/dev/fb0`) and the `-lcd.dtb` (panel → a
DRM connector + scanout). To show a real desktop on that panel, see
`tests/weston/run.sh`, which boots the `core-image-weston` rootfs and
software-renders Weston (pixman) onto the LCDIFv3 output.

## On a successful boot with the LCD dtb

Instead of "no connector", the kernel log shows the LCDIFv3 binding a CRTC and
creating `fb0`, e.g.:

```
imx-drm display-subsystem: bound imx-lcdifv3-crtc.0 (ops lcdifv3_crtc_ops)
imx-drm display-subsystem: bound 4ae10000.dsi (ops dw_mipi_dsi_imx_ops)
imx-drm display-subsystem: [drm] fb0: imx-drmdrmfb frame buffer device
```
