import { sha256Hex } from '../fw/bootstrap';
import type { StagedRepoAssets } from './payload';

export interface AssetManifestEntry {
  size: number;
  sha256: string;
}

export interface AssetManifest {
  version: number;
  files: Record<string, AssetManifestEntry>;
}

const REQUIRED_KEYS = [
  'ie_auto.sh',
  'nest_rtsp',
  'nest_onvif',
  'dropbear',
  'zImage',
  'initrd_base.bin',
] as const;

export async function fetchManifest(baseUrl = './assets'): Promise<AssetManifest> {
  const res = await fetch(`${baseUrl}/manifest.json`, { cache: 'no-store' });
  if (!res.ok) {
    throw new Error(
      'Staged camera binaries were not found in ./assets/. Run ./scripts/prepare-web-assets.sh before starting the dev server or building.',
    );
  }
  const manifest = (await res.json()) as AssetManifest;
  for (const key of REQUIRED_KEYS) {
    if (!manifest.files?.[key]) {
      throw new Error(`Asset manifest is missing required entry "${key}".`);
    }
  }
  return manifest;
}

async function fetchAndVerify(
  baseUrl: string,
  name: string,
  expected: AssetManifestEntry,
): Promise<Uint8Array> {
  const res = await fetch(`${baseUrl}/${name}`, { cache: 'no-store' });
  if (!res.ok) {
    throw new Error(`Failed to fetch ${name} (HTTP ${res.status}).`);
  }
  const data = new Uint8Array(await res.arrayBuffer());
  if (data.byteLength !== expected.size) {
    throw new Error(
      `Asset ${name} size mismatch: expected ${expected.size} bytes, got ${data.byteLength}.`,
    );
  }
  const actualSha = await sha256Hex(data);
  if (actualSha !== expected.sha256) {
    throw new Error(`Asset ${name} SHA-256 verification failed.`);
  }
  return data;
}

export async function loadStagedAssets(
  baseUrl = './assets',
  onLog?: (msg: string) => void,
): Promise<StagedRepoAssets> {
  const manifest = await fetchManifest(baseUrl);
  onLog?.('Loading and verifying staged ARMv6 binaries and RAM kernel...');

  const [ieAutoSh, nestRtsp, nestOnvif, dropbear, zImage, initrdGzip, version] =
    await Promise.all([
      fetchAndVerify(baseUrl, 'ie_auto.sh', manifest.files['ie_auto.sh']),
      fetchAndVerify(baseUrl, 'nest_rtsp', manifest.files['nest_rtsp']),
      fetchAndVerify(baseUrl, 'nest_onvif', manifest.files['nest_onvif']),
      fetchAndVerify(baseUrl, 'dropbear', manifest.files['dropbear']),
      fetchAndVerify(baseUrl, 'zImage', manifest.files['zImage']),
      fetchAndVerify(baseUrl, 'initrd_base.bin', manifest.files['initrd_base.bin']),
      manifest.files['VERSION']
        ? fetchAndVerify(baseUrl, 'VERSION', manifest.files['VERSION'])
        : Promise.resolve(undefined),
    ]);

  onLog?.('All staged camera assets verified against manifest SHA-256.');
  return { ieAutoSh, nestRtsp, nestOnvif, dropbear, zImage, initrdGzip, version };
}
