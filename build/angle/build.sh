#!/bin/sh
# Build both Android ABIs from a gclient-synced checkout at our exact pin.
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
. "$repo/packaging/ci/pins.env"
src=$(CDPATH= cd -- "${1:?usage: build.sh ANGLE_CHECKOUT [STAGE]}" && pwd)
stage=${2:-"$src/stage"}
test "$(git -C "$src" rev-parse HEAD)" = "$ANGLE_SHA" || {
  echo "ANGLE checkout does not match ANGLE_SHA" >&2; exit 1;
}
for abi in x86_64 x86; do
  case "$abi" in
    x86_64) out=AndroidX64; libdir=lib64; class=ELF64; machine='Advanced Micro Devices X86-64' ;;
    x86) out=AndroidX86; libdir=lib; class=ELF32; machine='Intel 80386' ;;
  esac
  mkdir -p "$src/out/$out" "$stage/vendor/$libdir/egl"
  cp "$repo/build/angle/args-$abi.gn" "$src/out/$out/args.gn"
  (cd "$src" && buildtools/linux64/gn gen "out/$out")
  # GN must actually link the decoder, not merely accept an unused argument.
  rg -q 'ANGLE_HAS_ASTCENC' "$src/out/$out/obj/libANGLE.ninja"
  ninja -C "$src/out/$out" -j "${JOBS:-8}" libEGL libGLESv1_CM libGLESv2
  for lib in libEGL_angle.so libGLESv1_CM_angle.so libGLESv2_angle.so; do
    binary="$src/out/$out/$lib"
    readelf -h "$binary" | grep -q "Class:.*$class"
    readelf -h "$binary" | grep -q "Machine:.*$machine"
    readelf -d "$binary" | grep -q "SONAME.*$lib"
    cp "$binary" "$stage/vendor/$libdir/egl/$lib"
  done
done
