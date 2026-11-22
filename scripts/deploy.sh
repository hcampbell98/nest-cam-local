#!/usr/bin/env bash
set -euo pipefail

# Deploy compiled binaries, boot scripts, and optional device configs to
# /mnt/dropcam on a rooted Gen 1 Nest Cam Outdoor over Dropbear SSH.
#
# Usage:
#   ./scripts/deploy.sh root@<CAMERA_IP> [--restart | --reboot]
#
# Notes:
#   - Expects Dropbear SSH to be running on the target camera.
#     For a freshly rooted camera that only has Telnet, see README.md Step 5.
#   - Uploads onvif.conf and audio.mode only if those local files exist in the
#     repository root.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

if [ $# -lt 1 ]; then
  echo "Usage: $0 root@<CAMERA_IP> [--restart | --reboot]" >&2
  exit 1
fi

TARGET="$1"
ACTION="${2:-}"

if [ -n "$ACTION" ] && [ "$ACTION" != "--restart" ] && [ "$ACTION" != "--reboot" ]; then
  echo "Error: unknown option '$ACTION'. Use --restart or --reboot." >&2
  exit 1
fi

REQUIRED_FILES=(
  "$REPO_ROOT/ie_auto.sh"
  "$REPO_ROOT/local_start.sh"
  "$REPO_ROOT/nest_rtsp/nest_rtsp"
  "$REPO_ROOT/nest_onvif/nest_onvif"
  "$REPO_ROOT/dropbear/dropbear"
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
chmod 755 \
  "$STAGE_DIR/ie_auto.sh" \
  "$STAGE_DIR/local_start.sh" \
  "$STAGE_DIR/nest_rtsp" \
  "$STAGE_DIR/nest_onvif" \
  "$STAGE_DIR/dropbear"

if [ -f "$REPO_ROOT/onvif.conf" ]; then
  echo "[+] Including local onvif.conf"
  cp "$REPO_ROOT/onvif.conf" "$STAGE_DIR/onvif.conf"
  chmod 600 "$STAGE_DIR/onvif.conf"
else
  echo "[i] Local onvif.conf not found; keeping existing /mnt/dropcam/onvif.conf on camera."
fi

if [ -f "$REPO_ROOT/audio.mode" ]; then
  echo "[+] Including local audio.mode"
  cp "$REPO_ROOT/audio.mode" "$STAGE_DIR/audio.mode"
  chmod 644 "$STAGE_DIR/audio.mode"
fi

# Note: Dropbear here is built without sftp-server, so stream a tarball over ssh
# and swap binaries via temp files so running executables are not overwritten in place.
echo "[+] Uploading payload to $TARGET:/mnt/dropcam ..."
tar -C "$STAGE_DIR" -cf - . | ssh "$TARGET" '
  set -e
  tmp="/mnt/dropcam/.deploy.$$"
  rm -rf "$tmp"
  mkdir -p "$tmp"
  tar -C "$tmp" -xf -
  for f in ie_auto.sh local_start.sh nest_rtsp nest_onvif dropbear onvif.conf audio.mode; do
    if [ -f "$tmp/$f" ]; then
      mv -f "$tmp/$f" "/mnt/dropcam/$f"
    fi
  done
  rm -rf "$tmp"
  sync
  ls -la /mnt/dropcam
'

case "$ACTION" in
  --restart)
    echo "[+] Restarting nest_rtsp and nest_onvif..."
    ssh "$TARGET" 'killall nest_rtsp nest_onvif 2>/dev/null || true'
    ;;
  --reboot)
    echo "[+] Rebooting camera..."
    ssh "$TARGET" 'reboot' || true
    ;;
  *)
    echo "[+] Deploy complete."
    echo "    To apply binary updates immediately: ssh $TARGET 'killall nest_rtsp nest_onvif'"
    echo "    To test ie_auto.sh or dropbear changes: ssh $TARGET 'reboot'"
    ;;
esac
