import { sha256Hex } from '../fw/bootstrap';
import { generateShadowFile } from '../crypto/shadow';
import type { CpioEntry } from '../initrd/cpio';
import { UBIATT2048_BIN } from './ubiatt2048';

export type MirrorMode = 0 | 1 | 2 | 3;
export type AudioMode = 'off' | 'mic' | 'speaker' | 'two-way';

export interface CameraConfig {
  wifiSsid: string;
  wifiPassword: string;
  onvifUser: string;
  onvifPassword: string;
  rootPassword: string;
  mirrorMode: MirrorMode;
  audioMode: AudioMode;
  maxBitrateKbps: number;
  minBitrateKbps: number;
  allowNonHwver5: boolean;
  dryRun: boolean;
}

export interface StagedRepoAssets {
  ieAutoSh: Uint8Array;
  nestRtsp: Uint8Array;
  nestOnvif: Uint8Array;
  dropbear: Uint8Array;
  zImage: Uint8Array;
  initrdGzip: Uint8Array;
  version?: Uint8Array;
}

const ENC = new TextEncoder();

function escapeWpaString(s: string): string {
  return s.replace(/\\/g, '\\\\').replace(/"/g, '\\"');
}

export function buildWpaSupplicantConf(ssid: string, password: string): string {
  if (!ssid.trim()) throw new Error('Wi-Fi SSID cannot be empty.');
  if (password.length < 8 || password.length > 63) {
    throw new Error('WPA2 Wi-Fi password must be between 8 and 63 characters.');
  }
  return [
    'ctrl_interface=/var/run/wpa_supplicant',
    'update_config=1',
    '',
    'network={',
    `\tssid="${escapeWpaString(ssid)}"`,
    `\tpsk="${escapeWpaString(password)}"`,
    '\tkey_mgmt=WPA-PSK',
    '}',
    '',
  ].join('\n');
}

export function buildOnvifConf(user: string, password: string): string {
  if (!/^[A-Za-z0-9._-]{1,32}$/.test(user)) {
    throw new Error('ONVIF/RTSP username must use letters, digits, ., _, or -.');
  }
  if (!password || /[\r\n:]/.test(password)) {
    throw new Error('ONVIF/RTSP password cannot be empty or contain colons/newlines.');
  }
  return `${user}:${password}\n`;
}

export function buildCameraConf(config: CameraConfig): string {
  const maxBps = Math.max(600, Math.min(4000, Math.trunc(config.maxBitrateKbps))) * 1000;
  const minBps = Math.max(300, Math.min(maxBps / 1000, Math.trunc(config.minBitrateKbps))) * 1000;
  const mirror = config.mirrorMode;
  return [
    '# Gen 1 Nest Cam Outdoor runtime settings (preserved across firmware updates).',
    `MIRROR_MODE=${mirror}`,
    `MAX_BITRATE=${maxBps}`,
    `MIN_BITRATE=${minBps}`,
    'EXTRA_RTSP_ARGS=""',
    '',
  ].join('\n');
}

export function buildLocalStartSh(config?: CameraConfig): string {
  const maxBps = config
    ? Math.max(600, Math.min(4000, Math.trunc(config.maxBitrateKbps))) * 1000
    : 2200000;
  const minBps = config
    ? Math.max(300, Math.min(maxBps / 1000, Math.trunc(config.minBitrateKbps))) * 1000
    : 600000;
  const mirror = config ? config.mirrorMode : 3;
  return [
    '#!/bin/sh',
    'export PATH=/sbin:/usr/sbin:/usr/local/bin:/usr/bin:/bin',
    '',
    '# Turn off the RGB status LED after boot.',
    'for led in /sys/class/leds/lp5521:channel*; do',
    '  [ -d "$led" ] || continue',
    "  printf '0\\n' >\"$led/brightness\"",
    "  printf '0\\n' >\"$led/led_current\"",
    'done',
    '',
    '# Start ONVIF Profile S + WS-Discovery daemon (port 80 + UDP 3702) once wlan0 has an IPv4 address',
    'if [ -x /mnt/dropcam/nest_onvif ] && ! pidof nest_onvif >/dev/null; then',
    '  (',
    '    i=0',
    "    while [ \"$i\" -lt 20 ] && ! ifconfig wlan0 2>/dev/null | grep -q 'inet addr:'; do",
    '      sleep 1',
    '      i=$((i + 1))',
    '    done',
    '    while true; do',
    '      /mnt/dropcam/nest_onvif -p 80 -r 554 -i wlan0 -c /mnt/dropcam/onvif.conf >/tmp/onvif.log 2>&1',
    '      sleep 2',
    '    done',
    '  ) &',
    'fi',
    '',
    '# Start RTSP with the configured audio directions.',
    'while true; do',
    '  if ! grep -qw iav /proc/devices; then',
    '    if ! /usr/local/bin/init.sh --ar0330 >/tmp/nest-encoder-init.log 2>&1; then',
    '      sleep 2',
    '      continue',
    '    fi',
    '  fi',
    '  set --',
    '  case "$(cat /mnt/dropcam/audio.mode 2>/dev/null)" in',
    '    mic) set -- --mic ;;',
    '    speaker) set -- --speaker ;;',
    '    two-way) set -- --mic --speaker ;;',
    '  esac',
    `  MIRROR_MODE=${mirror}`,
    `  MAX_BITRATE=${maxBps}`,
    `  MIN_BITRATE=${minBps}`,
    '  EXTRA_RTSP_ARGS=""',
    '  if [ -f /mnt/dropcam/camera.conf ]; then',
    '    . /mnt/dropcam/camera.conf',
    '  fi',
    '  /mnt/dropcam/nest_rtsp -w 1920 -h 1080 -b "$MAX_BITRATE" --min-bitrate "$MIN_BITRATE" -p 554 -m "$MIRROR_MODE" $EXTRA_RTSP_ARGS "$@" >/dev/null 2>&1',
    '  sleep 2',
    'done',
    '',
  ].join('\n');
}

/**
 * Autonomous UBIFS installer script placed at /usr/bin/autoreflash.sh inside
 * the concatenated initramfs. The stock initrd /init automatically invokes it
 * when "autoreflash" is present in /proc/cmdline.
 *
 * Safety properties:
 *  - Verifies SHA-256 of every staged payload file before touching flash.
 *  - Mounts /dev/mtd9 as native UBIFS (ubi0:adc) without erasing the partition,
 *    preserving all factory calibration and identity files untouched.
 *  - Checks hwver == 5 unless explicitly overridden.
 *  - Installs files via atomic temp-file + mv, syncs, unmounts, detaches UBI,
 *    and reboots only on complete success. On error, stays in the 10.9.9.1
 *    USB Ethernet recovery shell with diagnostics in /tmp/install_error.log.
 */
export function buildAutoreflashScript(
  allowNonHwver5: boolean,
  dryRun = false,
): string {
  const hwverCheck = allowNonHwver5
    ? 'echo "[installer] hwver check bypassed by user option (found: $HWVER)" | tee -a "$LOG"'
    : `if [ "$HWVER" != "5" ]; then
  fail "Unsupported hardware version hwver='$HWVER' (expected 5 for Gen 1 Nest Cam Outdoor)."
fi`;

  const mountOpts = dryRun ? '-t ubifs -o ro' : '-t ubifs';
  const installBlock = dryRun
    ? `# 5. Dry-run mode: read-only verification only, zero flash writes
log "DRY-RUN PASSED: UBIFS mounted read-only, hwver=$HWVER verified, SHA256SUMS verified. No bytes written to flash."
umount "$MNT" || fail "umount $MNT failed."
ubidetach -d 0 /dev/ubi_ctrl 2>/dev/null || true
sync

log "Dry-run complete! Rebooting camera cleanly..."
sleep 1
reboot -f`
    : `# 5. Stage files atomically onto /mnt/adc (preserves all factory calibration files)
STAGE="$MNT/.webflash.$$"
rm -rf "$STAGE"
mkdir -p "$STAGE" || fail "Failed to create staging directory on $MNT."

cp "$PAYLOAD/ie_auto.sh" "$STAGE/ie_auto.sh" || fail "Copy ie_auto.sh failed."
cp "$PAYLOAD/local_start.sh" "$STAGE/local_start.sh" || fail "Copy local_start.sh failed."
cp "$PAYLOAD/nest_rtsp" "$STAGE/nest_rtsp" || fail "Copy nest_rtsp failed."
cp "$PAYLOAD/nest_onvif" "$STAGE/nest_onvif" || fail "Copy nest_onvif failed."
cp "$PAYLOAD/dropbear" "$STAGE/dropbear" || fail "Copy dropbear failed."
cp "$PAYLOAD/wpa_supplicant.conf" "$STAGE/wpa_supplicant.conf" || fail "Copy wpa_supplicant.conf failed."
cp "$PAYLOAD/onvif.conf" "$STAGE/onvif.conf" || fail "Copy onvif.conf failed."
cp "$PAYLOAD/camera.conf" "$STAGE/camera.conf" || fail "Copy camera.conf failed."
cp "$PAYLOAD/audio.mode" "$STAGE/audio.mode" || fail "Copy audio.mode failed."
cp "$PAYLOAD/VERSION" "$STAGE/VERSION" || fail "Copy VERSION failed."
cp "$PAYLOAD/shadow" "$STAGE/shadow" || fail "Copy shadow failed."

chmod 755 "$STAGE/ie_auto.sh" "$STAGE/local_start.sh" "$STAGE/nest_rtsp" "$STAGE/nest_onvif" "$STAGE/dropbear"
chmod 600 "$STAGE/onvif.conf" "$STAGE/shadow" "$STAGE/wpa_supplicant.conf"
chmod 644 "$STAGE/audio.mode" "$STAGE/camera.conf" "$STAGE/VERSION"

# Re-verify written copies on flash before swapping into place
(cd "$STAGE" && sha256sum -c "$PAYLOAD/SHA256SUMS") >> "$LOG" 2>&1 || fail "Flash readback SHA-256 check failed."

for f in ie_auto.sh local_start.sh nest_rtsp nest_onvif dropbear wpa_supplicant.conf onvif.conf camera.conf audio.mode VERSION shadow; do
  mv -f "$STAGE/$f" "$MNT/$f" || fail "Atomic move failed for $f."
done
rm -rf "$STAGE"

log "Installed all files cleanly. Writing install.log and unmounting..."
cp "$LOG" "$MNT/install.log" 2>/dev/null || true
sync
umount "$MNT" || fail "umount $MNT failed."
ubidetach -d 0 /dev/ubi_ctrl 2>/dev/null || true
sync

log "Installation complete! Rebooting camera into local mode..."
sleep 1
reboot -f`;

  return `#!/bin/sh
LOG="/tmp/install.log"
ERR="/tmp/install_error.log"
MNT="/mnt/adc"
PAYLOAD="/payload"

log() {
  echo "[nest-cam-local] $1" | tee -a "$LOG"
}

fail() {
  echo "[nest-cam-local] ERROR: $1" | tee -a "$LOG" > "$ERR"
  if mountpoint -q "$MNT" 2>/dev/null; then
    umount "$MNT" 2>/dev/null || true
  fi
  ubidetach -d 0 /dev/ubi_ctrl 2>/dev/null || true
  echo "[nest-cam-local] Leaving USB recovery shell active at 10.9.9.1 (telnet)." | tee -a "$LOG"
  exit 1
}

log "Starting autonomous UBIFS installer${dryRun ? ' (DRY-RUN READ-ONLY MODE)' : ''}..."

# 1. Verify staged payload integrity in RAM before touching NAND
if [ ! -f "$PAYLOAD/SHA256SUMS" ]; then
  fail "Missing $PAYLOAD/SHA256SUMS manifest."
fi
(cd "$PAYLOAD" && sha256sum -c SHA256SUMS) >> "$LOG" 2>&1 || fail "Payload SHA-256 verification failed."
log "Payload SHA-256 verification passed."

# 2. Bind Ambarella NAND driver and wait for /dev/mtd9 and /dev/ubi_ctrl
if [ -e /sys/bus/platform/drivers/ambarella-nand/bind ]; then
  echo ambarella-nand > /sys/bus/platform/drivers/ambarella-nand/bind 2>/dev/null || true
fi

i=0
while [ "$i" -lt 20 ]; do
  mdev -s 2>/dev/null || true
  [ -e /dev/mtd9 ] && [ -e /dev/ubi_ctrl ] && break
  sleep 1
  i=$((i + 1))
done

[ -e /dev/mtd9 ] || fail "/dev/mtd9 did not appear after binding ambarella-nand."
[ -e /dev/ubi_ctrl ] || fail "/dev/ubi_ctrl is missing."

if ! grep -q '^mtd9:' /proc/mtd; then
  fail "mtd9 not found in /proc/mtd."
fi
log "Found /dev/mtd9: $(grep '^mtd9:' /proc/mtd)"

# 3. Attach UBI on mtd9 with VID header offset 2048 (BusyBox 1.23 ubiattach omits -O 2048)
mkdir -p "$MNT"
if [ ! -e /dev/ubi0 ]; then
  if ! /usr/sbin/ubiatt2048 >> "$LOG" 2>&1; then
    if ! ubiattach -m 9 -d 0 /dev/ubi_ctrl >> "$LOG" 2>&1; then
      [ -e /dev/ubi0 ] || fail "UBI attach on mtd9 failed (is mtd9 UBIFS?)."
    fi
  fi
fi

sleep 1
mdev -s 2>/dev/null || true

if ! mount ${mountOpts} ubi0:adc "$MNT" >> "$LOG" 2>&1; then
  if ! mount ${mountOpts} ubi0_0 "$MNT" >> "$LOG" 2>&1; then
    fail "Could not mount UBIFS volume on mtd9."
  fi
fi
log "Mounted /dev/mtd9 UBIFS at $MNT."

# 4. Verify factory identity file hwver
[ -f "$MNT/hwver" ] || fail "Missing $MNT/hwver; refusing to modify unknown volume."
HWVER="$(tr -d '\\r\\n ' < "$MNT/hwver")"
${hwverCheck}
log "Verified camera hwver=$HWVER."

${installBlock}
`;
}

export async function buildOverlayEntries(
  config: CameraConfig,
  assets: StagedRepoAssets,
): Promise<{ entries: CpioEntry[]; manifest: string }> {
  const wpaConf = ENC.encode(buildWpaSupplicantConf(config.wifiSsid, config.wifiPassword));
  const onvifConf = ENC.encode(buildOnvifConf(config.onvifUser, config.onvifPassword));
  const cameraConf = ENC.encode(buildCameraConf(config));
  const audioMode = ENC.encode(`${config.audioMode}\n`);
  const versionData = assets.version ?? ENC.encode('1.1.0-local\n');
  const localStartSh = ENC.encode(buildLocalStartSh(config));
  const shadowFile = ENC.encode(await generateShadowFile(config.rootPassword));
  const autoreflashSh = ENC.encode(
    buildAutoreflashScript(config.allowNonHwver5, config.dryRun),
  );

  const payloadFiles: Array<{ name: string; data: Uint8Array; mode: number }> = [
    { name: 'ie_auto.sh', data: assets.ieAutoSh, mode: 0o100755 },
    { name: 'local_start.sh', data: localStartSh, mode: 0o100755 },
    { name: 'nest_rtsp', data: assets.nestRtsp, mode: 0o100755 },
    { name: 'nest_onvif', data: assets.nestOnvif, mode: 0o100755 },
    { name: 'dropbear', data: assets.dropbear, mode: 0o100755 },
    { name: 'wpa_supplicant.conf', data: wpaConf, mode: 0o100600 },
    { name: 'onvif.conf', data: onvifConf, mode: 0o100600 },
    { name: 'camera.conf', data: cameraConf, mode: 0o100644 },
    { name: 'audio.mode', data: audioMode, mode: 0o100644 },
    { name: 'VERSION', data: versionData, mode: 0o100644 },
    { name: 'shadow', data: shadowFile, mode: 0o100600 },
  ];

  const sums: string[] = [];
  for (const f of payloadFiles) {
    const digest = await sha256Hex(f.data);
    sums.push(`${digest}  ${f.name}`);
  }
  const manifest = `${sums.join('\n')}\n`;

  const entries: CpioEntry[] = [
    { path: 'usr', mode: 0o040755 },
    { path: 'usr/bin', mode: 0o040755 },
    { path: 'usr/sbin', mode: 0o040755 },
    { path: 'usr/sbin/ubiatt2048', mode: 0o100755, data: UBIATT2048_BIN },
    { path: 'usr/bin/autoreflash.sh', mode: 0o100755, data: autoreflashSh },
    { path: 'payload', mode: 0o040755 },
    ...payloadFiles.map((f) => ({
      path: `payload/${f.name}`,
      mode: f.mode,
      data: f.data,
    })),
    { path: 'payload/SHA256SUMS', mode: 0o100644, data: ENC.encode(manifest) },
  ];

  return { entries, manifest };
}
