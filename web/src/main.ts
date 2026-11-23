import './styles.css';
import {
  extractBootstrapFromHd2,
  V312_BLD_SIZE,
  V312_HAL_SIZE,
  type BootstrapImages,
} from './fw/bootstrap';
import { buildCpioNewc, buildMergedInitrd, gzipCompress } from './initrd/cpio';
import { AmbarellaLoader, A5S_AUTOREFLASH_CMDLINE } from './usb/ambarella';
import {
  AMBARELLA_A5S_PID,
  AMBARELLA_VID,
  isWebUsbSupported,
  MockUsbTransport,
  requestAmbarellaDevice,
  WebUsbTransport,
  type DeviceInfo,
  type UsbTransport,
} from './usb/transport';
import {
  buildOverlayEntries,
  type AudioMode,
  type CameraConfig,
  type MirrorMode,
} from './flasher/payload';
import { fetchManifest, loadStagedAssets, type AssetManifest } from './flasher/assets';
import { attachAccessibleValidation } from './ui/formValidation';

type WizardStep = 1 | 2 | 3 | 4;
type NvrTab = 'unifi' | 'frigate' | 'ha' | 'go2rtc';

interface AppState {
  step: WizardStep;
  transport: UsbTransport | null;
  deviceInfo: DeviceInfo | null;
  connectError: string | null;
  manifest: AssetManifest | null;
  manifestError: string | null;
  bootstrap: BootstrapImages | null;
  bootstrapName: string | null;
  bootstrapError: string | null;
  bootstrapBusy: boolean;
  config: CameraConfig;
  useFastBulk: boolean;
  flashing: boolean;
  finalizing: boolean;
  finalizeRemainingSec: number;
  finalizeTotalSec: number;
  dfuDisconnected: boolean;
  flashDone: boolean;
  flashError: string | null;
  progressLabel: string;
  progressSent: number;
  progressTotal: number;
  flashStartMs: number;
  logs: string[];
  nvrTab: NvrTab;
}

const state: AppState = {
  step: 1,
  transport: null,
  deviceInfo: null,
  connectError: null,
  manifest: null,
  manifestError: null,
  bootstrap: null,
  bootstrapName: null,
  bootstrapError: null,
  bootstrapBusy: false,
  config: {
    wifiSsid: '',
    wifiPassword: '',
    onvifUser: 'admin',
    onvifPassword: '',
    rootPassword: '',
    mirrorMode: 0,
    audioMode: 'off',
    maxBitrateKbps: 2200,
    minBitrateKbps: 600,
    allowNonHwver5: false,
    dryRun: false,
  },
  useFastBulk: true,
  flashing: false,
  finalizing: false,
  finalizeRemainingSec: 0,
  finalizeTotalSec: 30,
  dfuDisconnected: false,
  flashDone: false,
  flashError: null,
  progressLabel: 'Idle',
  progressSent: 0,
  progressTotal: 0,
  flashStartMs: 0,
  logs: [],
  nvrTab: 'unifi',
};

let cleanupValidation: (() => void) | null = null;
let finalizeTimerId: number | null = null;

function clearFinalizeTimer(): void {
  if (finalizeTimerId !== null) {
    window.clearInterval(finalizeTimerId);
    finalizeTimerId = null;
  }
}

function esc(s: string): string {
  return s
    .replace(/&/g, '&amp;')
    .replace(/</g, '&lt;')
    .replace(/>/g, '&gt;')
    .replace(/"/g, '&quot;');
}

function detectPlatform(): 'linux' | 'windows' | 'macos' {
  const ua = navigator.userAgent.toLowerCase();
  if (ua.includes('win')) return 'windows';
  if (ua.includes('mac')) return 'macos';
  return 'linux';
}

function formatHexId(n: number): string {
  return `0x${n.toString(16).padStart(4, '0')}`;
}

function appendLog(line: string): void {
  const ts = new Date().toLocaleTimeString('en-US', { hour12: false });
  state.logs.push(`[${ts}] ${line}`);
  const term = document.getElementById('flash-terminal');
  if (term) {
    term.textContent = state.logs.join('\n');
    term.scrollTop = term.scrollHeight;
  }
}

function updateProgressDom(): void {
  const pct =
    state.progressTotal > 0
      ? Math.min(100, Math.round((state.progressSent / state.progressTotal) * 100))
      : 0;
  const bar = document.getElementById('flash-progress') as HTMLProgressElement | null;
  const label = document.getElementById('flash-progress-label');
  const stats = document.getElementById('flash-progress-stats');
  if (bar) {
    bar.max = Math.max(1, state.progressTotal);
    bar.value = state.progressSent;
  }
  if (label) {
    label.textContent = `${state.progressLabel} (${pct}%)`;
  }
  if (stats && state.progressTotal > 0) {
    if (state.finalizing) {
      stats.textContent = `${state.finalizeRemainingSec}s remaining`;
    } else if (state.flashDone) {
      stats.textContent = 'Ready';
    } else {
      const elapsedSec = Math.max(0.1, (performance.now() - state.flashStartMs) / 1000);
      const kbps = state.progressSent / 1024 / elapsedSec;
      const sentMb = (state.progressSent / (1024 * 1024)).toFixed(2);
      const totalMb = (state.progressTotal / (1024 * 1024)).toFixed(2);
      stats.textContent = `${sentMb} / ${totalMb} MiB | ${kbps.toFixed(0)} KiB/s`;
    }
  }
}

function renderDfuDiagramSvg(): string {
  return `
  <svg viewBox="0 0 460 216" width="460" height="216" role="img" aria-label="Exploded side profile of Nest Cam Outdoor showing the black front faceplate, clear inner cap with cyan gasket, lens and IR ring, main PCB with top-edge DFU button, bell-shaped rear shell, and rear USB cable">
    <!-- Exploded alignment ticks in the gaps -->
    <line x1="33" y1="112" x2="45" y2="112" stroke="currentColor" stroke-opacity="0.28" stroke-width="1.2" stroke-dasharray="2.5 2.5"/>
    <line x1="83" y1="112" x2="101" y2="112" stroke="currentColor" stroke-opacity="0.28" stroke-width="1.2" stroke-dasharray="3 3"/>
    <line x1="181" y1="112" x2="205" y2="112" stroke="currentColor" stroke-opacity="0.24" stroke-width="1.2" stroke-dasharray="3 3"/>

    <!-- 1. Adhered black "nest" front faceplate (side profile) -->
    <g>
      <path d="M23 56 C17 60 16 86 16 112 C16 138 17 164 23 168 L28 168 L28 56 Z" fill="#111827" stroke="currentColor" stroke-opacity="0.65" stroke-width="1.5"/>
      <!-- Status LED window near top -->
      <circle cx="21" cy="74" r="2.2" fill="#3dd68c"/>
      <!-- Lens aperture recess -->
      <rect x="17" y="99" width="6" height="26" rx="2" fill="currentColor" fill-opacity="0.25"/>
    </g>

    <!-- 2. Friction-fit clear inner cap with cyan weather gasket -->
    <g>
      <path d="M60 56 C48 62 47 86 47 112 C47 138 48 162 60 168 L72 168 L72 56 Z" fill="currentColor" fill-opacity="0.06" stroke="currentColor" stroke-opacity="0.48" stroke-width="1.6"/>
      <!-- Inner lens window -->
      <rect x="49" y="96" width="12" height="32" rx="3" fill="currentColor" fill-opacity="0.14"/>
      <!-- Signature cyan silicone gasket ring -->
      <rect x="72" y="54" width="5" height="116" rx="2.5" fill="#38bdf8" fill-opacity="0.9"/>
      <!-- Subtle Wi-Fi antenna coax lead along bottom edge -->
      <path d="M75 158 C88 162 118 164 162 158" fill="none" stroke="#f7b955" stroke-opacity="0.55" stroke-width="1.2" stroke-dasharray="2 2"/>
    </g>

    <!-- 3. Lens barrel + IR LED ring + Main PCB (side profile) -->
    <g>
      <!-- Protruding camera lens barrel -->
      <rect x="106" y="97" width="14" height="30" rx="3" fill="currentColor" fill-opacity="0.22" stroke="currentColor" stroke-opacity="0.68" stroke-width="1.5"/>
      <rect x="120" y="90" width="16" height="44" rx="3" fill="currentColor" fill-opacity="0.14" stroke="currentColor" stroke-opacity="0.55" stroke-width="1.5"/>

      <!-- IR LED carrier plate + top/bottom IR LED domes -->
      <rect x="136" y="70" width="7" height="84" rx="2" fill="currentColor" fill-opacity="0.28" stroke="currentColor" stroke-opacity="0.55" stroke-width="1.2"/>
      <!-- Upper IR LEDs -->
      <path d="M136 74 L127 74 C123 74 123 82 127 82 L136 82 Z" fill="currentColor" fill-opacity="0.14" stroke="currentColor" stroke-opacity="0.62" stroke-width="1.2"/>
      <path d="M136 85 L128 85 C125 85 125 92 128 92 L136 92 Z" fill="currentColor" fill-opacity="0.14" stroke="currentColor" stroke-opacity="0.5" stroke-width="1.1"/>
      <!-- Lower IR LEDs -->
      <path d="M136 132 L128 132 C125 132 125 139 128 139 L136 139 Z" fill="currentColor" fill-opacity="0.14" stroke="currentColor" stroke-opacity="0.5" stroke-width="1.1"/>
      <path d="M136 142 L127 142 C123 142 123 150 127 150 L136 150 Z" fill="currentColor" fill-opacity="0.14" stroke="currentColor" stroke-opacity="0.62" stroke-width="1.2"/>

      <!-- Standoffs between IR board and Main PCB -->
      <line x1="143" y1="82" x2="162" y2="82" stroke="currentColor" stroke-opacity="0.45" stroke-width="2"/>
      <line x1="143" y1="142" x2="162" y2="142" stroke="currentColor" stroke-opacity="0.45" stroke-width="2"/>

      <!-- Circular Main PCB (side-edge slab) -->
      <rect x="162" y="54" width="9" height="116" rx="3" fill="#1e6b4a" fill-opacity="0.58" stroke="#3dd68c" stroke-opacity="0.78" stroke-width="1.5"/>
      <!-- Rear shield / SoC block on back of PCB -->
      <rect x="171" y="90" width="6" height="44" rx="1.5" fill="currentColor" fill-opacity="0.22" stroke="currentColor" stroke-opacity="0.4" stroke-width="1"/>

      <!-- Highlighted DFU tactile button on top-front edge of Main PCB -->
      <circle cx="159" cy="64" r="11" fill="#0d6efd" fill-opacity="0.24" stroke="#4da3ff" stroke-width="1.4" stroke-dasharray="2.5 2"/>
      <rect x="155" y="59" width="7" height="10" rx="1.8" fill="#4da3ff" stroke="#ffffff" stroke-opacity="0.88" stroke-width="1"/>
    </g>

    <!-- Internal harness hint between PCB and rear shell -->
    <path d="M177 106 C190 106 196 103 209 105" fill="none" stroke="currentColor" stroke-opacity="0.35" stroke-width="1.5"/>
    <path d="M177 118 C190 118 196 121 209 119" fill="none" stroke="#ff6b7a" stroke-opacity="0.55" stroke-width="1.4"/>

    <!-- 4. Iconic bell-shaped Nest Cam Outdoor housing + rear collar -->
    <g>
      <!-- Tapered white bell body -->
      <path d="M216 53 C256 55 292 68 306 89 L306 135 C292 156 256 169 216 171 A 7 59 0 0 1 216 53 Z" fill="currentColor" fill-opacity="0.08" stroke="currentColor" stroke-opacity="0.68" stroke-width="1.8" stroke-linejoin="round"/>
      <!-- Hollow front opening rim -->
      <ellipse cx="216" cy="112" rx="7" ry="59" fill="currentColor" fill-opacity="0.06" stroke="currentColor" stroke-opacity="0.48" stroke-width="1.5"/>
      <!-- Rear magnetic base collar -->
      <path d="M306 89 L326 91 C330 92 332 100 332 112 C332 124 330 132 326 133 L306 135 Z" fill="currentColor" fill-opacity="0.11" stroke="currentColor" stroke-opacity="0.62" stroke-width="1.6"/>
    </g>

    <!-- 5. Rear USB cable & PC plug -->
    <g>
      <path d="M332 112 C350 112 356 122 372 122 L386 122" fill="none" stroke="#3dd68c" stroke-width="2.6" stroke-linecap="round"/>
      <!-- USB plug body -->
      <rect x="386" y="112" width="22" height="20" rx="3.5" fill="#3dd68c" fill-opacity="0.2" stroke="#3dd68c" stroke-width="1.6"/>
      <rect x="408" y="116" width="10" height="12" rx="1.5" fill="none" stroke="#3dd68c" stroke-width="1.4"/>
    </g>

    <!-- Callout: DFU button -->
    <path d="M167 56 L188 26 L202 26" fill="none" stroke="#4da3ff" stroke-width="1.6" stroke-dasharray="3 2.5"/>
    <circle cx="167" cy="56" r="2.2" fill="#4da3ff"/>
    <text x="208" y="23" fill="#4da3ff" font-size="11.5" font-weight="700">DFU button (top of PCB)</text>
    <text x="208" y="37" fill="currentColor" fill-opacity="0.72" font-size="10.5">Hold while plugging in (2s)</text>

    <!-- Callout: PC USB -->
    <text x="402" y="150" text-anchor="middle" fill="#3dd68c" font-size="10.5" font-weight="600">PC USB</text>
    <text x="402" y="163" text-anchor="middle" fill="currentColor" fill-opacity="0.62" font-size="9.5">4255:0001</text>

    <!-- Bottom part labels -->
    <text x="22" y="196" text-anchor="middle" fill="currentColor" fill-opacity="0.62" font-size="9.5">Black</text>
    <text x="22" y="207" text-anchor="middle" fill="currentColor" fill-opacity="0.62" font-size="9.5">cover</text>
    <text x="64" y="196" text-anchor="middle" fill="currentColor" fill-opacity="0.62" font-size="9.5">Clear</text>
    <text x="64" y="207" text-anchor="middle" fill="currentColor" fill-opacity="0.62" font-size="9.5">cap</text>
    <text x="142" y="196" text-anchor="middle" fill="currentColor" fill-opacity="0.62" font-size="9.5">IR + Main PCB</text>
    <text x="268" y="196" text-anchor="middle" fill="currentColor" fill-opacity="0.62" font-size="9.5">Rear shell</text>
  </svg>`;
}

function renderOsUsbHelp(): string {
  const plat = detectPlatform();
  const forceOpen = state.connectError?.toLowerCase().includes('access denied') ? 'open' : '';
  if (plat === 'windows') {
    return `
      <details ${forceOpen}>
        <summary>Windows driver setup (one-time via Zadig)</summary>
        <ol class="checklist" style="margin-top:0.5rem">
          <li>With the camera plugged in in DFU mode, open <a href="https://zadig.akeo.ie/" target="_blank" rel="noreferrer">Zadig</a> -> <strong>Options -> List All Devices</strong>.</li>
          <li>Select the device with USB ID <code>4255 0001</code>.</li>
          <li>Select <strong>WinUSB</strong>, click <strong>Install / Replace Driver</strong>, then close Zadig and click Connect.</li>
        </ol>
      </details>`;
  }
  if (plat === 'linux') {
    return `
      <details ${forceOpen}>
        <summary>Linux USB permissions (one-time udev rule)</summary>
        <pre class="snippet" style="margin-top:0.5rem">echo 'SUBSYSTEM=="usb", ATTR{idVendor}=="4255", ATTR{idProduct}=="0001", MODE="0660", TAG+="uaccess"' | sudo tee /etc/udev/rules.d/99-ambarella-dfu.rules
sudo udevadm control --reload-rules &amp;&amp; sudo udevadm trigger</pre>
      </details>`;
  }
  return '';
}

function renderStep1(): string {
  const webUsbOk = isWebUsbSupported();
  const connected = state.deviceInfo !== null;

  return `
    <section class="card">
      <h2>1. Open Camera &amp; Enter DFU Mode</h2>
      <p class="subtitle">Expose the PCB DFU button inside your Gen 1 Nest Cam Outdoor and connect USB directly to your PC.</p>

      ${
        !webUsbOk
          ? `<div class="banner warn">
              <strong>WebUSB unavailable.</strong> Use desktop Chrome, Edge, or Brave (or try Simulation Mode below).
            </div>`
          : ''
      }

      ${
        state.manifestError
          ? `<div class="banner warn"><strong>Missing assets:</strong> ${esc(state.manifestError)}</div>`
          : ''
      }

      <div class="grid-2">
        <div>
          <ol class="checklist">
            <li><strong>Unplug the camera</strong>, then pry off the adhered <strong>black "nest" cover</strong> with a thin shim (gentle heat helps soften the adhesive).</li>
            <li><strong>Pry out the clear inner cap</strong> (friction fit). Go slowly and watch the <strong>Wi-Fi antenna lines</strong> around the edge so they stay attached.</li>
            <li><strong>Hold the top-edge PCB DFU button</strong> while plugging the camera's USB into your PC.</li>
            <li><strong>Release after 2 seconds.</strong> The front LED stays off (<code>4255:0001</code>).</li>
          </ol>
          ${renderOsUsbHelp()}
        </div>
        <div class="diagram-box">
          ${renderDfuDiagramSvg()}
        </div>
      </div>

      ${
        state.connectError
          ? `<div class="banner danger" style="margin-top:1rem"><strong>Connection failed:</strong> ${esc(state.connectError)}</div>`
          : ''
      }

      ${
        connected && state.deviceInfo
          ? `<div class="banner ok" style="margin-top:1rem">
              <strong>Connected:</strong> ${esc(state.deviceInfo.productName)}
              (<code>${formatHexId(state.deviceInfo.vendorId)}:${formatHexId(state.deviceInfo.productId)}</code>)
            </div>`
          : ''
      }

      <div class="actions">
        <button id="btn-connect-usb" class="btn primary" ${!webUsbOk ? 'disabled' : ''}>
          ${connected && !state.deviceInfo?.simulated ? 'Reconnect Camera' : 'Connect Camera'}
        </button>
        <button id="btn-connect-sim" class="btn">Simulate (No Hardware)</button>
        ${
          connected
            ? `<button id="btn-next-step2" class="btn primary">Continue -></button>`
            : ''
        }
      </div>
    </section>
  `;
}

function renderStep2(): string {
  const c = state.config;
  const isSim = Boolean(state.deviceInfo?.simulated);
  const canProceed = state.bootstrap !== null || isSim;

  return `
    <section class="card">
      <h2>2. Configure Camera</h2>
      <p class="subtitle">Credentials are packed locally in your browser and never leave your machine.</p>

      <form id="config-form" novalidate>
        <div class="grid-2">
          <div class="field">
            <label for="wifi-ssid">Wi-Fi SSID</label>
            <input
              type="text"
              id="wifi-ssid"
              name="wifiSsid"
              required
              minlength="1"
              maxlength="32"
              value="${esc(c.wifiSsid)}"
              data-error="wifi-ssid-error"
              placeholder="2.4 GHz network name"
            />
            <div id="wifi-ssid-error" class="error-msg">
              <span role="img" aria-label="Error">!</span> Enter a Wi-Fi SSID (1 to 32 chars).
            </div>
          </div>

          <div class="field">
            <label for="wifi-pass">Wi-Fi Password</label>
            <input
              type="password"
              id="wifi-pass"
              name="wifiPassword"
              required
              minlength="8"
              maxlength="63"
              autocomplete="off"
              value="${esc(c.wifiPassword)}"
              data-error="wifi-pass-error"
              placeholder="WPA2 passphrase (8 to 63 chars)"
            />
            <div id="wifi-pass-error" class="error-msg">
              <span role="img" aria-label="Error">!</span> Must be 8 to 63 characters.
            </div>
          </div>
        </div>

        <div class="grid-2">
          <div class="field">
            <label for="onvif-user">ONVIF / RTSP Username</label>
            <input
              type="text"
              id="onvif-user"
              name="onvifUser"
              required
              pattern="[A-Za-z0-9._-]{1,32}"
              value="${esc(c.onvifUser)}"
              data-error="onvif-user-error"
            />
            <div id="onvif-user-error" class="error-msg">
              <span role="img" aria-label="Error">!</span> Letters, numbers, ., _, or - only.
            </div>
          </div>

          <div class="field">
            <label for="onvif-pass">ONVIF / RTSP Password</label>
            <input
              type="password"
              id="onvif-pass"
              name="onvifPassword"
              required
              minlength="8"
              pattern="[^:]+"
              autocomplete="new-password"
              value="${esc(c.onvifPassword)}"
              data-error="onvif-pass-error"
              placeholder="8+ chars, no colons"
            />
            <div id="onvif-pass-error" class="error-msg">
              <span role="img" aria-label="Error">!</span> At least 8 characters with no colons (:).
            </div>
          </div>
        </div>

        <div class="grid-2">
          <div class="field">
            <label for="root-pass">Root SSH Password</label>
            <input
              type="password"
              id="root-pass"
              name="rootPassword"
              required
              minlength="8"
              autocomplete="new-password"
              value="${esc(c.rootPassword)}"
              data-error="root-pass-error"
              placeholder="8+ chars (Dropbear SSH)"
            />
            <div id="root-pass-error" class="error-msg">
              <span role="img" aria-label="Error">!</span> At least 8 characters.
            </div>
          </div>

          <div class="field">
            <label for="mirror-mode">Orientation</label>
            <select id="mirror-mode" name="mirrorMode">
              <option value="0" ${c.mirrorMode === 0 ? 'selected' : ''}>Normal (upright)</option>
              <option value="1" ${c.mirrorMode === 1 ? 'selected' : ''}>Horizontal mirror</option>
              <option value="2" ${c.mirrorMode === 2 ? 'selected' : ''}>Vertical flip</option>
              <option value="3" ${c.mirrorMode === 3 ? 'selected' : ''}>180° (upside-down mount)</option>
            </select>
          </div>
        </div>

        <div class="grid-2">
          <div class="field">
            <label for="audio-mode">Audio Mode</label>
            <select id="audio-mode" name="audioMode">
              <option value="off" ${c.audioMode === 'off' ? 'selected' : ''}>Off (default)</option>
              <option value="mic" ${c.audioMode === 'mic' ? 'selected' : ''}>Mic only</option>
              <option value="speaker" ${c.audioMode === 'speaker' ? 'selected' : ''}>Speaker only</option>
              <option value="two-way" ${c.audioMode === 'two-way' ? 'selected' : ''}>Two-way (mic + speaker)</option>
            </select>
          </div>

          <div class="field">
            <label for="max-bitrate">Max Bitrate (kbps)</label>
            <input
              type="number"
              id="max-bitrate"
              name="maxBitrateKbps"
              required
              min="800"
              max="4000"
              step="100"
              value="${c.maxBitrateKbps}"
              data-error="bitrate-error"
            />
            <div id="bitrate-error" class="error-msg">
              <span role="img" aria-label="Error">!</span> Choose 800 to 4000 kbps.
            </div>
          </div>
        </div>

        <div class="field">
          <label for="hd2-file">Bootstrap Firmware (<code>HD2-firmware.bin</code>)</label>
          <span class="hint">
            <a href="https://software.gopro.com/Firmware/HD2/HD2-firmware.bin" target="_blank" rel="noreferrer">Download GoPro Hero2 v312 (50 MB)</a>
            (<a href="https://web.archive.org/web/20250828183228/http://software.gopro.com/Firmware/HD2/HD2-firmware.bin" target="_blank" rel="noreferrer">mirror</a>)
            and drop it below. Used locally to extract the Ambarella A5s USB bootloader (verified by SHA-256).
          </span>
          <div id="hd2-dropzone" class="dropzone ${state.bootstrap ? 'verified' : ''}">
            ${
              state.bootstrapBusy
                ? '<strong>Verifying SHA-256...</strong>'
                : state.bootstrap
                  ? `<strong style="color:var(--success)">Verified: ${esc(state.bootstrapName || 'HD2-firmware.bin')}</strong>`
                  : `<div>Drop <code>HD2-firmware.bin</code> here, or choose file:</div>`
            }
            <input type="file" id="hd2-file" accept=".bin,application/octet-stream" />
          </div>
          ${
            state.bootstrapError
              ? `<div class="banner danger" style="margin-top:0.5rem">${esc(state.bootstrapError)}</div>`
              : ''
          }
          ${
            isSim && !state.bootstrap
              ? `<div class="hint" style="margin-top:0.35rem">Optional in Simulation Mode.</div>`
              : ''
          }
        </div>

        <details ${c.dryRun || !state.useFastBulk || c.allowNonHwver5 ? 'open' : ''}>
          <summary>Advanced Options</summary>
          <div style="display:grid;gap:0.55rem;margin-top:0.65rem">
            <label class="check-label">
              <input type="checkbox" id="dry-run-mode" ${c.dryRun ? 'checked' : ''} />
              <span>Read-only dry-run (verifies <code>mtd9</code> UBIFS without writing to flash)</span>
            </label>
            <label class="check-label">
              <input type="checkbox" id="use-fast-bulk" ${state.useFastBulk ? 'checked' : ''} />
              <span>Fast EP1 Bulk-OUT upload (~10s with hardware CRC32)</span>
            </label>
            <label class="check-label">
              <input type="checkbox" id="allow-non-hwver5" ${c.allowNonHwver5 ? 'checked' : ''} />
              <span>Allow non-<code>hwver=5</code> cameras</span>
            </label>
          </div>
        </details>

        <div class="actions">
          <button type="button" id="btn-back-step1" class="btn"><- Back</button>
          <button type="submit" id="btn-start-flash" class="btn primary" ${!canProceed ? 'disabled' : ''}>
            ${c.dryRun ? 'Run Dry-Run ->' : 'Flash Camera ->'}
          </button>
        </div>
      </form>
    </section>
  `;
}

function renderStep3(): string {
  const pct =
    state.progressTotal > 0
      ? Math.min(100, Math.round((state.progressSent / state.progressTotal) * 100))
      : 0;

  return `
    <section class="card">
      <h2>3. Flashing Camera</h2>
      <p class="subtitle">Keep USB connected until the on-camera install finishes (~40 seconds total).</p>

      <div class="progress-wrap">
        <div class="progress-head">
          <strong id="flash-progress-label">${esc(state.progressLabel)} (${pct}%)</strong>
          <span id="flash-progress-stats" class="mono" style="color:var(--muted)"></span>
        </div>
        <progress
          id="flash-progress"
          class="flash-progress"
          max="${Math.max(1, state.progressTotal)}"
          value="${state.progressSent}"
        >${pct}%</progress>
      </div>

      ${
        state.flashError
          ? `<div class="banner danger">
              <strong>Flashing stopped:</strong> ${esc(state.flashError)}
            </div>`
          : ''
      }

      ${
        state.finalizing
          ? `<div id="finalize-banner" class="banner warn">
              <strong id="finalize-title">Do not unplug USB yet (${state.finalizeRemainingSec}s)...</strong>
              <span id="finalize-detail">
                ${
                  state.dfuDisconnected
                    ? 'Camera exited DFU mode. '
                    : ''
                }${
                  state.config.dryRun
                    ? 'Verifying UBIFS read-only on mtd9 and rebooting without writing to flash.'
                    : 'Verifying checksums, installing to /mnt/dropcam, and rebooting into local mode.'
                }
              </span>
            </div>`
          : ''
      }

      ${
        state.flashDone
          ? `<div class="banner ok">
              <strong>${state.config.dryRun ? 'Dry-run complete! Safe to unplug USB.' : 'Flash complete! Safe to unplug USB.'}</strong>
              ${
                state.config.dryRun
                  ? 'Camera verified UBIFS read-only and rebooted without modifying flash.'
                  : 'Camera has finished installing to /mnt/dropcam and rebooted into local mode. You can now unplug USB and reassemble the front covers.'
              }
            </div>`
          : ''
      }

      <div id="flash-terminal" class="terminal mono">${esc(state.logs.join('\n'))}</div>

      <div class="actions">
        ${
          !state.flashing && !state.finalizing && state.flashError
            ? `<button id="btn-retry-flash" class="btn primary">Retry</button>
               <button id="btn-back-step2" class="btn"><- Back</button>`
            : ''
        }
        ${
          state.finalizing
            ? `<button id="btn-wait-finalize" class="btn primary" disabled>Waiting for camera (${state.finalizeRemainingSec}s)...</button>`
            : ''
        }
        ${
          state.flashDone
            ? `<button id="btn-next-step4" class="btn primary">NVR Setup -></button>`
            : ''
        }
      </div>
    </section>
  `;
}

function renderNvrSnippet(tab: NvrTab, user: string): string {
  if (tab === 'unifi') {
    return `
      <ol class="checklist">
        <li>Enable <strong>Settings -> System -> Advanced -> Discover Third-Party Cameras</strong>.</li>
        <li>Adopt <code>NestCamOutdoor</code> (port <code>80</code>) with username <code>${esc(user)}</code> and your ONVIF password.</li>
      </ol>`;
  }
  if (tab === 'frigate') {
    return `
      <pre class="snippet">go2rtc:
  streams:
    nest_outdoor:
      - rtsp://${esc(user)}:YOUR_ONVIF_PASSWORD@&lt;CAMERA_IP&gt;:554/stream1

cameras:
  nest_outdoor:
    ffmpeg:
      inputs:
        - path: rtsp://127.0.0.1:8554/nest_outdoor
          input_args: preset-rtsp-restream
          roles: [record, detect]</pre>`;
  }
  if (tab === 'ha') {
    return `
      <ol class="checklist">
        <li>Go to <strong>Settings -> Devices &amp; Services -> Add Integration -> ONVIF</strong>.</li>
        <li>Select <code>NestCamOutdoor</code> (port <code>80</code>) and sign in as <code>${esc(user)}</code>.</li>
      </ol>`;
  }
  return `
    <pre class="snippet">streams:
  nest_outdoor:
    - rtsp://${esc(user)}:YOUR_ONVIF_PASSWORD@&lt;CAMERA_IP&gt;:554/stream1</pre>`;
}

function renderStep4(): string {
  const user = state.config.onvifUser || 'admin';
  const rtspUrl = `rtsp://${user}:<PASSWORD>@<CAMERA_IP>:554/stream1`;
  const snapUrl = `http://<CAMERA_IP>/snapshot.jpg`;
  const onvifUrl = `http://<CAMERA_IP>/onvif/device_service`;
  const sshCmd = `ssh root@<CAMERA_IP>`;

  return `
    <section class="card">
      <h2>4. Ready to Connect</h2>
      <p class="subtitle">
        Reassemble the clear cap and black cover, power the camera normally, and find its IP on your router (<code>${esc(state.config.wifiSsid || 'Wi-Fi')}</code>). Open <code>http://&lt;CAMERA_IP&gt;/snapshot.jpg</code> to verify video.
      </p>

      <div class="code-row">
        <div><strong>RTSP:</strong> <code>${esc(rtspUrl)}</code></div>
        <button class="btn small copy-btn" data-copy="${esc(rtspUrl)}">Copy</button>
      </div>
      <div class="code-row">
        <div><strong>Snapshot:</strong> <code>${esc(snapUrl)}</code></div>
        <button class="btn small copy-btn" data-copy="${esc(snapUrl)}">Copy</button>
      </div>
      <div class="code-row">
        <div><strong>ONVIF:</strong> <code>${esc(onvifUrl)}</code></div>
        <button class="btn small copy-btn" data-copy="${esc(onvifUrl)}">Copy</button>
      </div>
      <div class="code-row">
        <div><strong>SSH:</strong> <code>${esc(sshCmd)}</code></div>
        <button class="btn small copy-btn" data-copy="${esc(sshCmd)}">Copy</button>
      </div>

      <h3 style="margin:1.15rem 0 0.5rem;font-size:1rem">NVR Setup</h3>
      <div class="tabs" role="tablist">
        <button class="tab-btn ${state.nvrTab === 'unifi' ? 'active' : ''}" data-tab="unifi">UniFi Protect</button>
        <button class="tab-btn ${state.nvrTab === 'frigate' ? 'active' : ''}" data-tab="frigate">Frigate</button>
        <button class="tab-btn ${state.nvrTab === 'ha' ? 'active' : ''}" data-tab="ha">Home Assistant</button>
        <button class="tab-btn ${state.nvrTab === 'go2rtc' ? 'active' : ''}" data-tab="go2rtc">go2rtc</button>
      </div>
      <div>${renderNvrSnippet(state.nvrTab, user)}</div>

      <div class="actions">
        <button id="btn-flash-another" class="btn">Flash Another Camera</button>
      </div>
    </section>
  `;
}

function render(): void {
  if (cleanupValidation) {
    cleanupValidation();
    cleanupValidation = null;
  }

  const app = document.getElementById('app');
  if (!app) return;

  const statusPill = state.deviceInfo
    ? `<span class="pill ok"><span class="pill-dot"></span>${
        state.deviceInfo.simulated ? 'Simulation' : 'Connected (4255:0001)'
      }</span>`
    : `<span class="pill"><span class="pill-dot"></span>Not Connected</span>`;

  const steps: Array<{ n: WizardStep; title: string; sub: string }> = [
    { n: 1, title: 'Connect', sub: 'DFU + WebUSB' },
    { n: 2, title: 'Configure', sub: 'Wi-Fi & Auth' },
    { n: 3, title: 'Flash', sub: 'UBIFS Install' },
    { n: 4, title: 'Done', sub: 'RTSP & ONVIF' },
  ];

  const stepBody =
    state.step === 1
      ? renderStep1()
      : state.step === 2
        ? renderStep2()
        : state.step === 3
          ? renderStep3()
          : renderStep4();

  app.innerHTML = `
    <main class="shell">
      <header class="topbar">
        <div class="brand">
          <img src="./icon.svg" alt="" />
          <div>
            <h1>nest-cam-local Flasher</h1>
            <p>Gen 1 Nest Cam Outdoor (Local RTSP + ONVIF)</p>
          </div>
        </div>
        ${statusPill}
      </header>

      <ol class="stepper" aria-label="Setup steps">
        ${steps
          .map((s) => {
            const done = s.n < state.step || (s.n === 4 && state.flashDone && state.step === 4);
            const active = s.n === state.step;
            const disabled =
              state.flashing ||
              state.finalizing ||
              (s.n > 1 && !state.deviceInfo) ||
              (s.n === 3 && !state.flashing && !state.finalizing && !state.flashDone && !state.flashError) ||
              (s.n === 4 && !state.flashDone);
            return `
              <li>
                <button
                  type="button"
                  class="step-btn ${active ? 'active' : ''} ${done ? 'done' : ''}"
                  data-jump-step="${s.n}"
                  ${disabled ? 'disabled' : ''}
                >
                  <span class="step-num">${done ? '✓' : s.n}</span>
                  <span class="step-meta">
                    <strong>${s.title}</strong>
                    <span>${s.sub}</span>
                  </span>
                </button>
              </li>`;
          })
          .join('')}
      </ol>

      ${stepBody}

      <p class="footer-note">
        100% client-side WebUSB flasher. Preserves factory calibration on <code>mtd9</code>.
      </p>
    </main>
  `;

  wireEvents();
  if (state.step === 2) {
    const form = document.getElementById('config-form');
    if (form) cleanupValidation = attachAccessibleValidation(form);
  }
  if (state.step === 3) {
    updateProgressDom();
  }
}

async function handleConnectWebUsb(): Promise<void> {
  state.connectError = null;
  try {
    if (state.transport) {
      await state.transport.close();
    }
    const usbDev = await requestAmbarellaDevice();
    const transport = new WebUsbTransport(usbDev);
    await transport.open();
    state.transport = transport;
    state.deviceInfo = transport.getDeviceInfo();
    state.step = 2;
  } catch (err) {
    const raw = err instanceof Error ? err.message : String(err);
    if (raw.toLowerCase().includes('access denied')) {
      const plat = detectPlatform();
      state.connectError =
        plat === 'windows'
          ? `${raw} Install WinUSB on 4255:0001 via Zadig below, close Zadig, and try again.`
          : plat === 'linux'
            ? `${raw} Install the udev rule below, reload udev, and replug in DFU mode.`
            : raw;
    } else {
      state.connectError = raw;
    }
  }
  render();
}

async function handleConnectSimulation(): Promise<void> {
  state.connectError = null;
  if (state.transport) {
    await state.transport.close();
  }
  const transport = new MockUsbTransport({ simulatedDelayEvery: 4096 });
  await transport.open();
  state.transport = transport;
  state.deviceInfo = transport.getDeviceInfo();
  state.step = 2;
  render();
}

async function handleHd2File(file: File): Promise<void> {
  state.bootstrapBusy = true;
  state.bootstrapError = null;
  render();
  try {
    const bytes = new Uint8Array(await file.arrayBuffer());
    const images = await extractBootstrapFromHd2(bytes);
    state.bootstrap = images;
    state.bootstrapName = file.name;
  } catch (err) {
    state.bootstrap = null;
    state.bootstrapName = null;
    state.bootstrapError = err instanceof Error ? err.message : String(err);
  } finally {
    state.bootstrapBusy = false;
    render();
  }
}

function readConfigFromForm(form: HTMLFormElement): void {
  const fd = new FormData(form);
  state.config.wifiSsid = String(fd.get('wifiSsid') ?? '');
  state.config.wifiPassword = String(fd.get('wifiPassword') ?? '');
  state.config.onvifUser = String(fd.get('onvifUser') ?? 'admin').trim();
  state.config.onvifPassword = String(fd.get('onvifPassword') ?? '');
  state.config.rootPassword = String(fd.get('rootPassword') ?? '');
  state.config.mirrorMode = Number(fd.get('mirrorMode') ?? 3) as MirrorMode;
  state.config.audioMode = String(fd.get('audioMode') ?? 'off') as AudioMode;
  state.config.maxBitrateKbps = Number(fd.get('maxBitrateKbps') ?? 2200);
  const fastBulkBox = document.getElementById('use-fast-bulk') as HTMLInputElement | null;
  if (fastBulkBox) state.useFastBulk = fastBulkBox.checked;
  const hwverBox = document.getElementById('allow-non-hwver5') as HTMLInputElement | null;
  if (hwverBox) state.config.allowNonHwver5 = hwverBox.checked;
  const dryRunBox = document.getElementById('dry-run-mode') as HTMLInputElement | null;
  if (dryRunBox) state.config.dryRun = dryRunBox.checked;
}

function startFinalizeCountdown(isSim: boolean): void {
  clearFinalizeTimer();
  const totalSec = isSim ? 3 : 30;
  state.finalizing = true;
  state.finalizeTotalSec = totalSec;
  state.finalizeRemainingSec = totalSec;
  state.progressLabel = state.config.dryRun ? 'Verifying on camera' : 'Installing on camera';
  state.progressTotal = totalSec;
  state.progressSent = 0;
  render();

  const updateFinalizeDom = (): void => {
    const elapsed = state.finalizeTotalSec - state.finalizeRemainingSec;
    const pct = Math.min(100, Math.round((elapsed / Math.max(1, state.finalizeTotalSec)) * 100));
    const bar = document.getElementById('flash-progress') as HTMLProgressElement | null;
    const label = document.getElementById('flash-progress-label');
    const stats = document.getElementById('flash-progress-stats');
    const title = document.getElementById('finalize-title');
    const waitBtn = document.getElementById('btn-wait-finalize');
    if (bar) {
      bar.max = state.finalizeTotalSec;
      bar.value = elapsed;
    }
    if (label) {
      label.textContent = `${state.progressLabel} (${pct}%)`;
    }
    if (stats) {
      stats.textContent = `${state.finalizeRemainingSec}s remaining`;
    }
    if (title) {
      title.textContent = `Do not unplug USB yet (${state.finalizeRemainingSec}s)...`;
    }
    if (waitBtn) {
      waitBtn.textContent = `Waiting for camera (${state.finalizeRemainingSec}s)...`;
    }
  };

  updateFinalizeDom();
  finalizeTimerId = window.setInterval(() => {
    state.finalizeRemainingSec = Math.max(0, state.finalizeRemainingSec - 1);
    state.progressSent = state.finalizeTotalSec - state.finalizeRemainingSec;
    if (state.finalizeRemainingSec <= 0) {
      clearFinalizeTimer();
      state.finalizing = false;
      state.flashDone = true;
      state.progressLabel = 'Complete';
      state.progressSent = state.progressTotal;
      appendLog(
        state.config.dryRun
          ? 'Dry-run window complete. Camera rebooted without modifying flash; safe to unplug USB.'
          : 'Install window complete! Camera has rebooted into local mode. Safe to unplug USB.',
      );
      render();
      return;
    }
    updateFinalizeDom();
  }, 1000);
}

async function runFlash(): Promise<void> {
  if (!state.transport) return;
  const isSim = Boolean(state.deviceInfo?.simulated);

  clearFinalizeTimer();
  state.step = 3;
  state.flashing = true;
  state.finalizing = false;
  state.dfuDisconnected = false;
  state.flashDone = false;
  state.flashError = null;
  state.logs = [];
  state.progressLabel = 'Preparing payload';
  state.progressSent = 0;
  state.progressTotal = 1;
  state.flashStartMs = performance.now();
  render();

  const usbApi =
    !isSim && typeof navigator !== 'undefined' && 'usb' in navigator ? navigator.usb : null;
  const onUsbDisconnect = (ev: USBConnectionEvent): void => {
    if (ev.device.vendorId === AMBARELLA_VID && ev.device.productId === AMBARELLA_A5S_PID) {
      if (!state.dfuDisconnected) {
        state.dfuDisconnected = true;
        appendLog('Camera exited DFU mode (4255:0001 disconnected). RAM installer running...');
        const detail = document.getElementById('finalize-detail');
        if (detail && !detail.textContent?.includes('exited DFU')) {
          detail.textContent = `Camera exited DFU mode. ${detail.textContent ?? ''}`;
        }
      }
    }
  };
  usbApi?.addEventListener('disconnect', onUsbDisconnect);

  try {
    appendLog(
      isSim
        ? 'Starting simulated flash.'
        : `Connected: ${formatHexId(AMBARELLA_VID)}:${formatHexId(AMBARELLA_A5S_PID)}`,
    );

    let bld: Uint8Array;
    let hal: Uint8Array;
    if (state.bootstrap) {
      bld = state.bootstrap.bld;
      hal = state.bootstrap.hal;
    } else if (isSim) {
      bld = new Uint8Array(V312_BLD_SIZE);
      hal = new Uint8Array(V312_HAL_SIZE);
    } else {
      throw new Error('HD2-firmware.bin is required before flashing.');
    }

    let assets;
    try {
      assets = await loadStagedAssets('./assets', appendLog);
    } catch (assetErr) {
      if (!isSim) throw assetErr;
      const enc = new TextEncoder();
      const emptyBaseGz = await gzipCompress(buildCpioNewc([]));
      assets = {
        ieAutoSh: enc.encode('#!/bin/sh\n'),
        nestRtsp: new Uint8Array(4096),
        nestOnvif: new Uint8Array(4096),
        dropbear: new Uint8Array(8192),
        zImage: new Uint8Array(32768),
        initrdGzip: emptyBaseGz,
      };
    }

    appendLog('Packing config, SHA-512 root shadow, and UBIFS installer...');
    const { entries } = await buildOverlayEntries(state.config, assets);
    const { initrd, compressedSize } = await buildMergedInitrd(assets.initrdGzip, entries);
    appendLog(`Custom initrd ready (${compressedSize.toLocaleString()} B).`);

    const loader = new AmbarellaLoader(state.transport);
    state.flashStartMs = performance.now();

    await loader.bootLinuxFlasher({
      bld,
      hal,
      zImage: assets.zImage,
      initrd,
      cmdline: A5S_AUTOREFLASH_CMDLINE,
      useFastBulk: state.useFastBulk,
      skipDelays: isSim,
      onLog: appendLog,
      onProgress: (p) => {
        state.progressLabel = p.label;
        state.progressSent = p.bytesSent;
        state.progressTotal = p.bytesTotal;
        updateProgressDom();
      },
    });

    await state.transport.close();
    state.flashing = false;
    startFinalizeCountdown(isSim);
    setTimeout(() => {
      usbApi?.removeEventListener('disconnect', onUsbDisconnect);
    }, 5000);
    return;
  } catch (err) {
    usbApi?.removeEventListener('disconnect', onUsbDisconnect);
    state.flashError = err instanceof Error ? err.message : String(err);
    appendLog(`ERROR: ${state.flashError}`);
    state.flashing = false;
    render();
  }
}

function wireEvents(): void {
  document.querySelectorAll<HTMLButtonElement>('[data-jump-step]').forEach((btn) => {
    btn.addEventListener('click', () => {
      const target = Number(btn.dataset.jumpStep) as WizardStep;
      if (target >= 1 && target <= 4 && !btn.disabled) {
        state.step = target;
        render();
      }
    });
  });

  document.getElementById('btn-connect-usb')?.addEventListener('click', () => {
    void handleConnectWebUsb();
  });
  document.getElementById('btn-connect-sim')?.addEventListener('click', () => {
    void handleConnectSimulation();
  });
  document.getElementById('btn-next-step2')?.addEventListener('click', () => {
    state.step = 2;
    render();
  });

  const form = document.getElementById('config-form') as HTMLFormElement | null;
  if (form) {
    form.addEventListener('submit', (e) => {
      e.preventDefault();
      readConfigFromForm(form);
      if (!form.checkValidity()) {
        form.reportValidity();
        form.querySelector<HTMLElement>(':invalid')?.focus();
        return;
      }
      if (!state.bootstrap && !state.deviceInfo?.simulated) {
        state.bootstrapError = 'Please select HD2-firmware.bin before flashing.';
        render();
        return;
      }
      void runFlash();
    });

    const hd2Input = document.getElementById('hd2-file') as HTMLInputElement | null;
    hd2Input?.addEventListener('change', () => {
      readConfigFromForm(form);
      const file = hd2Input.files?.[0];
      if (file) void handleHd2File(file);
    });

    const dropzone = document.getElementById('hd2-dropzone');
    if (dropzone) {
      dropzone.addEventListener('dragover', (e) => {
        e.preventDefault();
        dropzone.classList.add('dragover');
      });
      dropzone.addEventListener('dragleave', () => {
        dropzone.classList.remove('dragover');
      });
      dropzone.addEventListener('drop', (e) => {
        e.preventDefault();
        dropzone.classList.remove('dragover');
        readConfigFromForm(form);
        const file = e.dataTransfer?.files?.[0];
        if (file) void handleHd2File(file);
      });
    }

    document.getElementById('btn-back-step1')?.addEventListener('click', () => {
      readConfigFromForm(form);
      state.step = 1;
      render();
    });

    document.getElementById('dry-run-mode')?.addEventListener('change', () => {
      readConfigFromForm(form);
      const btn = document.getElementById('btn-start-flash');
      if (btn) btn.textContent = state.config.dryRun ? 'Run Dry-Run ->' : 'Flash Camera ->';
    });
  }

  document.getElementById('btn-retry-flash')?.addEventListener('click', () => {
    void runFlash();
  });
  document.getElementById('btn-back-step2')?.addEventListener('click', () => {
    state.step = 2;
    render();
  });
  document.getElementById('btn-next-step4')?.addEventListener('click', () => {
    state.step = 4;
    render();
  });
  document.getElementById('btn-flash-another')?.addEventListener('click', () => {
    clearFinalizeTimer();
    state.transport = null;
    state.deviceInfo = null;
    state.finalizing = false;
    state.dfuDisconnected = false;
    state.flashDone = false;
    state.flashError = null;
    state.logs = [];
    state.step = 1;
    render();
  });

  document.querySelectorAll<HTMLButtonElement>('[data-tab]').forEach((btn) => {
    btn.addEventListener('click', () => {
      state.nvrTab = btn.dataset.tab as NvrTab;
      render();
    });
  });

  document.querySelectorAll<HTMLButtonElement>('.copy-btn').forEach((btn) => {
    btn.addEventListener('click', async () => {
      const val = btn.dataset.copy ?? '';
      try {
        await navigator.clipboard.writeText(val);
        const orig = btn.textContent;
        btn.textContent = 'Copied!';
        setTimeout(() => {
          btn.textContent = orig;
        }, 1200);
      } catch {
        btn.textContent = 'Copy';
      }
    });
  });
}

async function init(): Promise<void> {
  render();
  try {
    state.manifest = await fetchManifest('./assets');
    state.manifestError = null;
  } catch (err) {
    state.manifestError = err instanceof Error ? err.message : String(err);
  }
  render();
}

void init();
