#!/usr/bin/env bash
set -euo pipefail

# Build a compact modern-crypto Dropbear SSH binary (Curve25519 + Ed25519 +
# ChaCha20-Poly1305) cross-compiled for ARMv6 soft-float and linked against
# the Gen 1 Nest Cam Outdoor's glibc 2.13 rootfs.
#
# Usage:
#   ./dropbear/build.sh [ROOTFS_PATH]
#
# Optional environment variables:
#   DROPBEAR_VER   Dropbear release version to download (default: 2025.87)
#   DROPBEAR_SRC   Path to an already-extracted Dropbear source directory
#   CRTDIR         Host ARM toolchain CRT directory (default: /usr/arm-linux-gnueabi/lib)

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

ROOTFS="${1:-${ROOTFS:-$REPO_ROOT/workspace/bak_extracted/86976456/rootfs}}"
DROPBEAR_VER="${DROPBEAR_VER:-2025.88}"
CRTDIR="${CRTDIR:-/usr/arm-linux-gnueabi/lib}"
BUILD_DIR="$REPO_ROOT/workspace/dropbear-build"

CC="arm-linux-gnueabi-gcc"
STRIP="arm-linux-gnueabi-strip"
READELF="arm-linux-gnueabi-readelf"

for tool in "$CC" "$STRIP" "$READELF" make tar; do
  if ! command -v "$tool" >/dev/null 2>&1; then
    echo "Error: required tool '$tool' not found in PATH." >&2
    exit 1
  fi
done

for lib in \
  "$ROOTFS/lib/libc-2.13.so" \
  "$ROOTFS/lib/libcrypt-2.13.so" \
  "$ROOTFS/lib/libutil-2.13.so" \
  "$ROOTFS/lib/ld-2.13.so"; do
  if [ ! -f "$lib" ]; then
    echo "Error: missing camera library: $lib" >&2
    echo "Pass your extracted camera rootfs path as the first argument or set ROOTFS." >&2
    exit 1
  fi
done

mkdir -p "$BUILD_DIR"

if [ -n "${DROPBEAR_SRC:-}" ]; then
  SRC_DIR="$(cd "$DROPBEAR_SRC" && pwd)"
else
  TARBALL="$BUILD_DIR/dropbear-${DROPBEAR_VER}.tar.bz2"
  SRC_DIR="$BUILD_DIR/dropbear-${DROPBEAR_VER}"
  if [ ! -f "$TARBALL" ]; then
    URL="https://matt.ucc.asn.au/dropbear/releases/dropbear-${DROPBEAR_VER}.tar.bz2"
    echo "[+] Downloading Dropbear ${DROPBEAR_VER}..."
    curl -fsSL "$URL" -o "$TARBALL"
  fi
  rm -rf "$SRC_DIR"
  tar -xjf "$TARBALL" -C "$BUILD_DIR"
fi

echo "[+] Applying localoptions.h and compiling glibc 2.13 compatibility shim..."
cp "$SCRIPT_DIR/localoptions.h" "$SRC_DIR/localoptions.h"

COMMON_CFLAGS="-march=armv6 -mfloat-abi=soft -D_TIME_BITS=32 -D_FILE_OFFSET_BITS=32 -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0 -Os -ffunction-sections -fdata-sections"

"$CC" $COMMON_CFLAGS -c "$SCRIPT_DIR/glibc213_compat.c" -o "$BUILD_DIR/glibc213_compat.o"

# Link directly against the camera's glibc 2.13 shared libraries so no newer
# GLIBC_2.33+ symbols enter the binary.
TARGET_LDFLAGS="-march=armv6 -mfloat-abi=soft -nostdlib -Wl,--gc-sections -Wl,--build-id=none -Wl,-dynamic-linker,/lib/ld-linux.so.3 $CRTDIR/crt1.o $CRTDIR/crti.o $BUILD_DIR/glibc213_compat.o"
TARGET_LIBS="$ROOTFS/lib/libc-2.13.so $ROOTFS/lib/libcrypt-2.13.so $ROOTFS/lib/libutil-2.13.so $ROOTFS/lib/ld-2.13.so -lgcc $CRTDIR/crtn.o"

pushd "$SRC_DIR" >/dev/null
echo "[+] Configuring Dropbear for arm-linux-gnueabi..."
./configure \
  --host=arm-linux-gnueabi \
  --disable-zlib \
  --disable-lastlog \
  --disable-utmp \
  --disable-utmpx \
  --disable-wtmp \
  --disable-wtmpx \
  --disable-harden \
  --disable-pututline \
  --disable-pututxline \
  ac_cv_sys_file_offset_bits=no \
  CC="$CC" \
  CFLAGS="$COMMON_CFLAGS" \
  LDFLAGS="$TARGET_LDFLAGS" \
  LIBS="$TARGET_LIBS"

echo "[+] Building dropbear..."
make -j"$(nproc)" PROGRAMS="dropbear" MULTI=0
popd >/dev/null

cp "$SRC_DIR/dropbear" "$SCRIPT_DIR/dropbear"
"$STRIP" --strip-all -R .comment -R .note.ABI-tag "$SCRIPT_DIR/dropbear"

echo "[+] Verifying ELF dependencies and GLIBC symbol versions:"
"$READELF" -d "$SCRIPT_DIR/dropbear" | grep NEEDED || true
"$READELF" -V "$SCRIPT_DIR/dropbear" | grep 'Name: GLIBC' || true
ls -lh "$SCRIPT_DIR/dropbear"
echo "[+] Done: $SCRIPT_DIR/dropbear"
