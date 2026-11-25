#!/usr/bin/env bash
set -euo pipefail

# Build a self-contained, SHA-256 verified firmware update bundle (.tar.gz)
# for upload via the camera's local web UI (http://<CAMERA_IP>/update).
#
# Usage:
#   ./scripts/build-update-bundle.sh [OUTPUT_PATH]

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

VERSION="1.0.0-local"
if [ -f "$REPO_ROOT/VERSION" ]; then
  VERSION="$(tr -d '\r\n ' < "$REPO_ROOT/VERSION")"
fi

OUT_PATH="${1:-$REPO_ROOT/nest-cam-local-update-${VERSION}.tar.gz}"

REQUIRED_FILES=(
  "$REPO_ROOT/ie_auto.sh"
  "$REPO_ROOT/local_start.sh"
  "$REPO_ROOT/nest_rtsp/nest_rtsp"
  "$REPO_ROOT/nest_onvif/nest_onvif"
  "$REPO_ROOT/dropbear/dropbear"
  "$REPO_ROOT/VERSION"
)

for f in "${REQUIRED_FILES[@]}"; do
  if [ ! -f "$f" ]; then
    echo "Error: missing required file: $f" >&2
    echo "Build nest_rtsp, nest_onvif, and dropbear first (see README.md Step 3)." >&2
    exit 1
  fi
done

STAGE_DIR="$(mktemp -d)"
trap 'rm -rf "$STAGE_DIR"' EXIT

cp "$REPO_ROOT/ie_auto.sh" "$STAGE_DIR/ie_auto.sh"
cp "$REPO_ROOT/local_start.sh" "$STAGE_DIR/local_start.sh"
cp "$REPO_ROOT/nest_rtsp/nest_rtsp" "$STAGE_DIR/nest_rtsp"
cp "$REPO_ROOT/nest_onvif/nest_onvif" "$STAGE_DIR/nest_onvif"
cp "$REPO_ROOT/dropbear/dropbear" "$STAGE_DIR/dropbear"
cp "$REPO_ROOT/VERSION" "$STAGE_DIR/VERSION"

chmod 755 \
  "$STAGE_DIR/ie_auto.sh" \
  "$STAGE_DIR/local_start.sh" \
  "$STAGE_DIR/nest_rtsp" \
  "$STAGE_DIR/nest_onvif" \
  "$STAGE_DIR/dropbear"
chmod 644 "$STAGE_DIR/VERSION"

(
  cd "$STAGE_DIR"
  sha256sum ie_auto.sh local_start.sh nest_rtsp nest_onvif dropbear VERSION > SHA256SUMS
  chmod 644 SHA256SUMS
  tar --owner=0 --group=0 -czf "$OUT_PATH" \
    SHA256SUMS VERSION ie_auto.sh local_start.sh nest_rtsp nest_onvif dropbear
)

echo "[+] Built update bundle: $OUT_PATH"
ls -lh "$OUT_PATH"
sha256sum "$OUT_PATH"
