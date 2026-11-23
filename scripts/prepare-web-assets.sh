#!/usr/bin/env bash
set -euo pipefail

# Stage compiled ARMv6 binaries, boot scripts, and RAM kernel assets into
# web/public/assets/ so the WebUSB flasher can bundle them into its custom initrd.
#
# Usage:
#   ./scripts/prepare-web-assets.sh [GOPRO_USB_TOOLS_DIR]

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
OUT_DIR="$REPO_ROOT/web/public/assets"
GP_DIR="${1:-$REPO_ROOT/Dropcam-Rooter/gopro-usb-tools}"

mkdir -p "$OUT_DIR"

declare -A ASSETS=(
  ["ie_auto.sh"]="$REPO_ROOT/ie_auto.sh"
  ["local_start.sh"]="$REPO_ROOT/local_start.sh"
  ["nest_rtsp"]="$REPO_ROOT/nest_rtsp/nest_rtsp"
  ["nest_onvif"]="$REPO_ROOT/nest_onvif/nest_onvif"
  ["dropbear"]="$REPO_ROOT/dropbear/dropbear"
  ["zImage"]="$GP_DIR/zImage"
)

INITRD_LZMA="$GP_DIR/initrd.lzma"

MISSING=0
for name in "${!ASSETS[@]}"; do
  src="${ASSETS[$name]}"
  if [ ! -f "$src" ]; then
    echo "Missing required asset: $src" >&2
    MISSING=1
  fi
done
if [ ! -f "$INITRD_LZMA" ]; then
  echo "Missing required asset: $INITRD_LZMA" >&2
  MISSING=1
fi

if [ "$MISSING" -ne 0 ]; then
  echo "" >&2
  echo "Build nest_rtsp, nest_onvif, and dropbear first, and pass the directory" >&2
  echo "containing zImage and initrd.lzma (from gopro-usb-tools) if not at:" >&2
  echo "  $GP_DIR" >&2
  exit 1
fi

for name in "${!ASSETS[@]}"; do
  cp "${ASSETS[$name]}" "$OUT_DIR/$name"
done

# Convert initrd.lzma to initrd_base.bin (gzipped cpio with a .bin extension so
# static hosts and Vite dev server never add Content-Encoding: gzip and
# transparently decompress it during fetch()).
rm -f "$OUT_DIR/initrd.lzma" "$OUT_DIR/initrd.cpio.gz"
python3 - "$INITRD_LZMA" "$OUT_DIR" <<'PY'
import gzip, hashlib, json, lzma, os, sys

initrd_lzma, out_dir = sys.argv[1], sys.argv[2]
raw_cpio = lzma.decompress(open(initrd_lzma, "rb").read())
assert raw_cpio[:6] == b"070701", "Expected SVR4 newc cpio inside initrd.lzma"

# Strip the accidental self-recursive 2.69 MB "initrd" cpio copy left inside
# upstream initrd.lzma; this cuts ~1.36 MB (~340k WebUSB transfers) per flash.
cleaned = bytearray()
pos = 0
while pos < len(raw_cpio):
    assert raw_cpio[pos:pos+6] == b"070701", f"Bad cpio magic at {pos}"
    ns = int(raw_cpio[pos+94:pos+102], 16)
    fs = int(raw_cpio[pos+54:pos+62], 16)
    name = raw_cpio[pos+110:pos+110+ns-1].decode("utf-8", "replace")
    end = (((pos + 110 + ns + 3) & ~3) + fs + 3) & ~3
    if name != "initrd":
        cleaned.extend(raw_cpio[pos:end])
    pos = end
    if name == "TRAILER!!!":
        break

gz_path = os.path.join(out_dir, "initrd_base.bin")
with open(gz_path, "wb") as f:
    f.write(gzip.compress(bytes(cleaned), compresslevel=9, mtime=0))

files = ["ie_auto.sh", "local_start.sh", "nest_rtsp", "nest_onvif", "dropbear", "zImage", "initrd_base.bin"]
manifest = {"version": 2, "files": {}}
for name in files:
    p = os.path.join(out_dir, name)
    data = open(p, "rb").read()
    manifest["files"][name] = {
        "size": len(data),
        "sha256": hashlib.sha256(data).hexdigest(),
    }
with open(os.path.join(out_dir, "manifest.json"), "w", encoding="utf-8") as f:
    json.dump(manifest, f, indent=2)
    f.write("\n")
PY

echo "[+] Staged web flasher assets into $OUT_DIR:"
ls -lh "$OUT_DIR"
