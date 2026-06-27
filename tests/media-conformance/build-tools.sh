#!/usr/bin/env bash
#
# Cross-build the three aarch64 conformance binaries this sweep needs into
# MEDIA_BIN_DIR (default ./bin): modetest (libdrm), v4l2-compliance (v4l-utils),
# and the v4l2_cap pipeline-setup oracle (in-repo). Needs an aarch64 toolchain,
# meson/ninja, and internet (clones libdrm + v4l-utils). Run once, then run.sh.
#
# The guest rootfs already carries libstdc++/libm/libgcc_s/libc, so the tools are
# left dynamically linked against those; libdrm is statically embedded.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
CROSS=${CROSS:-aarch64-linux-gnu-}
OUT=${MEDIA_BIN_DIR:-$HERE/bin}
WORK=${WORK:-$HERE/.build}
LIBDRM_TAG=${LIBDRM_TAG:-libdrm-2.4.120}
V4L_TAG=${V4L_TAG:-v4l-utils-1.28.1}
mkdir -p "$OUT" "$WORK"

cat > "$WORK/cross.ini" <<EOF
[binaries]
c = '${CROSS}gcc'
cpp = '${CROSS}g++'
ar = '${CROSS}ar'
strip = '${CROSS}strip'
pkg-config = 'pkg-config'
[host_machine]
system = 'linux'
cpu_family = 'aarch64'
cpu = 'armv8-a'
endian = 'little'
EOF

echo ">>> v4l2_cap (in-repo oracle)"
"${CROSS}gcc" -O2 -Wall -static -o "$OUT/v4l2_cap" "$REPO/tests/camera-imx93/v4l2_cap.c"

echo ">>> modetest (libdrm $LIBDRM_TAG)"
[ -d "$WORK/drm" ] || git clone --depth 1 -b "$LIBDRM_TAG" \
    https://gitlab.freedesktop.org/mesa/drm.git "$WORK/drm"
rm -rf "$WORK/drm/build"
meson setup "$WORK/drm/build" "$WORK/drm" --cross-file "$WORK/cross.ini" \
    --default-library=static \
    -Dintel=disabled -Dradeon=disabled -Damdgpu=disabled -Dnouveau=disabled \
    -Dvmwgfx=disabled -Dfreedreno=disabled -Dvc4=disabled -Detnaviv=disabled \
    -Dcairo-tests=disabled -Dman-pages=disabled -Dvalgrind=disabled \
    -Dtests=true -Dinstall-test-programs=true
ninja -C "$WORK/drm/build" tests/modetest/modetest
cp "$WORK/drm/build/tests/modetest/modetest" "$OUT/modetest"

echo ">>> v4l2-compliance (v4l-utils $V4L_TAG)"
[ -d "$WORK/v4l-utils" ] || git clone --depth 1 -b "$V4L_TAG" \
    https://git.linuxtv.org/v4l-utils.git "$WORK/v4l-utils"
rm -rf "$WORK/v4l-utils/build"
meson setup "$WORK/v4l-utils/build" "$WORK/v4l-utils" --cross-file "$WORK/cross.ini" \
    --default-library=static \
    -Dbpf=disabled -Dgconv=disabled -Djpeg=disabled -Dlibdvbv5=disabled \
    -Dqv4l2=disabled -Dqvidcap=disabled -Dv4l2-tracer=disabled -Ddoxygen-doc=disabled
ninja -C "$WORK/v4l-utils/build" utils/v4l2-compliance/v4l2-compliance
cp "$WORK/v4l-utils/build/utils/v4l2-compliance/v4l2-compliance" "$OUT/v4l2-compliance"

echo
echo "built into $OUT:"
for b in v4l2_cap modetest v4l2-compliance; do
    printf '  %-18s ' "$b"; file "$OUT/$b" | grep -o 'ARM aarch64' || echo "MISSING"
done
echo "now: MEDIA_BIN_DIR=$OUT ./run.sh all"
