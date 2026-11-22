import { describe, expect, it } from 'vitest';
import { existsSync, readFileSync } from 'node:fs';
import { resolve } from 'node:path';
import { gunzipSync } from 'node:zlib';

import {
  extractBootstrapFromHd2,
  HAL_PATCH_V312,
  HD2_FIRMWARE_SIZE,
  V312_BLD_SHA256,
  V312_HAL_SHA256,
} from '../fw/bootstrap';
import { generateShadowFile, sha512Crypt } from '../crypto/shadow';
import { buildCpioNewc, buildMergedInitrd, gzipCompress, mergeCpioNewc } from '../initrd/cpio';
import {
  A5S_ATAGS_ADDR,
  A5S_BLD_ADDR,
  A5S_BLD_JUMP_PATCH_ADDR,
  A5S_BLD_JUMP_PATCH_INSN,
  A5S_HAL_ADDR,
  A5S_INITRD_ADDR,
  A5S_KERNEL_ADDR,
  A5S_TRAMPOLINE_ADDR,
  AmbarellaLoader,
  HERO2_ALT_DDR_INIT_SEQ,
} from '../usb/ambarella';
import { MockUsbTransport } from '../usb/transport';
import {
  buildAutoreflashScript,
  buildLocalStartSh,
  buildOnvifConf,
  buildOverlayEntries,
  buildWpaSupplicantConf,
  type CameraConfig,
} from '../flasher/payload';

describe('bootstrap extraction (prepare-bootstrap port)', () => {
  it('has all 141 v312 HAL patch entries', () => {
    expect(HAL_PATCH_V312.length).toBe(141);
  });

  it('rejects wrong file sizes', async () => {
    await expect(extractBootstrapFromHd2(new Uint8Array(128))).rejects.toThrow(
      /Invalid HD2-firmware\.bin size/,
    );
  });

  it('rejects wrong SHA-256 even with exact file size', async () => {
    const fake = new Uint8Array(HD2_FIRMWARE_SIZE);
    await expect(extractBootstrapFromHd2(fake)).rejects.toThrow(/Unexpected HD2-firmware\.bin SHA-256/);
  });

  it('matches C prepare-bootstrap outputs byte-for-byte when HD2-firmware.bin is present locally', async () => {
    const localFw = resolve(process.cwd(), '../Dropcam-Rooter/gopro-usb-tools/HD2-firmware.bin');
    if (!existsSync(localFw)) return;
    const bytes = new Uint8Array(readFileSync(localFw));
    const res = await extractBootstrapFromHd2(bytes);
    expect(res.bldSha256).toBe(V312_BLD_SHA256);
    expect(res.halSha256).toBe(V312_HAL_SHA256);
  });
});

describe('glibc SHA-512-crypt ($6$) password hashing', () => {
  it('matches Ulrich Drepper / glibc official test vector', async () => {
    /* Official test vector from Ulrich Drepper SHA-crypt specification */
    const hash = await sha512Crypt('Hello world!', 'saltstring', 5000);
    expect(hash).toBe(
      '$6$saltstring$svn8UoSVapNtMuq1ukKS4tPQd8iKwSMHWjl/O817G3uBnIFNjnQJuesI68u4OTLiBFdcbYEdFCoEOfaS35inz1',
    );
  });

  it('generates a valid shadow file with no blank root password', async () => {
    const shadow = await generateShadowFile('SuperSecret123!');
    const lines = shadow.trim().split('\n');
    expect(lines[0]).toMatch(/^root:\$6\$[./0-9A-Za-z]{16}\$[./0-9A-Za-z]{86}:/);
    expect(lines[0]).not.toMatch(/^root::/);
    expect(lines.some((l) => l.startsWith('default:*:'))).toBe(true);
  });
});

describe('POSIX newc cpio and concatenated initramfs', () => {
  it('writes valid 070701 records, 4-byte aligned, with TRAILER!!!', () => {
    const enc = new TextEncoder();
    const archive = buildCpioNewc([
      { path: 'payload', mode: 0o040755 },
      { path: 'payload/hello.txt', mode: 0o100644, data: enc.encode('hello') },
    ]);
    const text = new TextDecoder('latin1').decode(archive);
    expect(text.startsWith('070701')).toBe(true);
    expect(text).toContain('payload/hello.txt');
    expect(text).toContain('TRAILER!!!');
    expect(archive.byteLength % 4).toBe(0);
  });

  it('merges overlay into a single gzipped cpio without duplicate paths', async () => {
    const enc = new TextEncoder();
    const baseCpio = buildCpioNewc([
      { path: 'init', mode: 0o100755, data: enc.encode('#!/bin/sh\n') },
      { path: 'usr/bin/autoreflash.sh', mode: 0o100755, data: enc.encode('OLD_SCRIPT') },
    ]);
    const baseGz = await gzipCompress(baseCpio);
    const { initrd } = await buildMergedInitrd(baseGz, [
      { path: 'usr/bin/autoreflash.sh', mode: 0o100755, data: enc.encode('NEW_SCRIPT') },
      { path: 'payload/test.txt', mode: 0o100644, data: enc.encode('ok') },
    ]);
    expect(initrd[0]).toBe(0x1f);
    expect(initrd[1]).toBe(0x8b);
    expect(initrd.byteLength % 4).toBe(0);
    const inflated = gunzipSync(initrd).toString('latin1');
    expect(inflated).toContain('NEW_SCRIPT');
    expect(inflated).not.toContain('OLD_SCRIPT');
    expect(inflated).toContain('payload/test.txt');
    expect(inflated.match(/usr\/bin\/autoreflash\.sh/g)?.length).toBe(1);
    expect(inflated.match(/TRAILER!!!/g)?.length).toBe(1);
  });

  it('merges cleanly with the real staged initrd_base.bin without duplicate autoreflash.sh or reordered parent dirs', async () => {
    const stagedInitrd = resolve(process.cwd(), 'public/assets/initrd_base.bin');
    if (!existsSync(stagedInitrd)) return;
    const baseGz = new Uint8Array(readFileSync(stagedInitrd));
    const enc = new TextEncoder();
    const { initrd } = await buildMergedInitrd(baseGz, [
      { path: 'usr', mode: 0o040755 },
      { path: 'usr/bin', mode: 0o040755 },
      { path: 'usr/bin/autoreflash.sh', mode: 0o100755, data: enc.encode('#!/bin/sh\necho NEST_LOCAL_TEST\n') },
      { path: 'payload', mode: 0o040755 },
      { path: 'payload/SHA256SUMS', mode: 0o100644, data: enc.encode('ok\n') },
    ]);
    const inflated = new Uint8Array(gunzipSync(initrd));
    const dec = new TextDecoder('latin1');
    const paths: string[] = [];
    let pos = 0;
    while (pos + 110 <= inflated.byteLength) {
      expect(dec.decode(inflated.subarray(pos, pos + 6))).toBe('070701');
      const fs = parseInt(dec.decode(inflated.subarray(pos + 54, pos + 62)), 16);
      const ns = parseInt(dec.decode(inflated.subarray(pos + 94, pos + 102)), 16);
      const name = dec.decode(inflated.subarray(pos + 110, pos + 110 + ns - 1));
      const dataStart = (pos + 110 + ns + 3) & ~3;
      pos = (dataStart + fs + 3) & ~3;
      if (name === 'TRAILER!!!') break;
      paths.push(name);
    }
    expect(paths.filter((p) => p === 'usr/bin/autoreflash.sh')).toEqual(['usr/bin/autoreflash.sh']);
    expect(paths.filter((p) => p === 'usr')).toEqual(['usr']);
    expect(paths.filter((p) => p === 'usr/bin')).toEqual(['usr/bin']);
    expect(paths.indexOf('usr')).toBeLessThan(paths.indexOf('usr/sbin/telnetd'));
    expect(paths.indexOf('usr/bin')).toBeLessThan(paths.indexOf('usr/bin/sha256sum'));
    expect(paths.indexOf('payload')).toBeLessThan(paths.indexOf('payload/SHA256SUMS'));
    expect(paths).not.toContain('initrd');
    expect(dec.decode(inflated)).toContain('NEST_LOCAL_TEST');
    expect(() => mergeCpioNewc(inflated, [])).not.toThrow();
  });
});

describe('payload and UBIFS autoreflash script generation', () => {
  const sampleCfg: CameraConfig = {
    wifiSsid: 'My "Test" WiFi',
    wifiPassword: 'password1234',
    onvifUser: 'admin',
    onvifPassword: 'stream-password',
    rootPassword: 'root-ssh-password',
    mirrorMode: 0,
    audioMode: 'mic',
    maxBitrateKbps: 2000,
    minBitrateKbps: 600,
    allowNonHwver5: false,
    dryRun: false,
  };

  it('escapes quotes in wpa_supplicant.conf and validates onvif.conf', () => {
    const wpa = buildWpaSupplicantConf(sampleCfg.wifiSsid, sampleCfg.wifiPassword);
    expect(wpa).toContain('ssid="My \\"Test\\" WiFi"');
    expect(wpa).toContain('psk="password1234"');
    expect(buildOnvifConf('admin', 'secret')).toBe('admin:secret\n');
    expect(() => buildOnvifConf('admin', 'bad:pass')).toThrow();
  });

  it('applies chosen mirror and bitrate settings to local_start.sh', () => {
    const sh = buildLocalStartSh(sampleCfg);
    expect(sh).toContain('-b 2000000 --min-bitrate 600000 -p 554 -m 0');
  });

  it('builds a safe UBIFS autoreflash script with VID-offset-2048 attach, ubidetach -d 0, and hwver=5 checks', () => {
    const sh = buildAutoreflashScript(false, false);
    expect(sh).toContain('sha256sum -c SHA256SUMS');
    expect(sh).toContain('/usr/sbin/ubiatt2048');
    expect(sh).toContain('ubidetach -d 0 /dev/ubi_ctrl');
    expect(sh).toContain('mount -t ubifs ubi0:adc');
    expect(sh).toContain('"$HWVER" != "5"');
    expect(sh).not.toContain('flash_eraseall');
  });

  it('builds a read-only dry-run script that writes zero bytes to flash', () => {
    const sh = buildAutoreflashScript(false, true);
    expect(sh).toContain('mount -t ubifs -o ro ubi0:adc');
    expect(sh).toContain('DRY-RUN PASSED');
    expect(sh).not.toContain('mv -f');
    expect(sh).not.toContain('.webflash.');
  });

  it('builds complete overlay entries with SHA256SUMS manifest', async () => {
    const enc = new TextEncoder();
    const { entries, manifest } = await buildOverlayEntries(sampleCfg, {
      ieAutoSh: enc.encode('#!/bin/sh\n'),
      nestRtsp: new Uint8Array([1, 2, 3, 4]),
      nestOnvif: new Uint8Array([5, 6, 7, 8]),
      dropbear: new Uint8Array([9, 10, 11, 12]),
      zImage: new Uint8Array(16),
      initrdGzip: new Uint8Array(16),
    });
    const paths = entries.map((e) => e.path);
    expect(paths).toContain('usr/sbin/ubiatt2048');
    expect(paths).toContain('usr/bin/autoreflash.sh');
    expect(paths).toContain('payload/SHA256SUMS');
    expect(paths).toContain('payload/shadow');
    expect(paths).toContain('payload/onvif.conf');
    expect(manifest.trim().split('\n').length).toBe(9);
  });
});

describe('Ambarella A5s WebUSB DFU loader', () => {
  it('uploads and verifies images via the ARMv6 EP1 Bulk-OUT fast loader by default', async () => {
    const mock = new MockUsbTransport();
    await mock.open();
    const loader = new AmbarellaLoader(mock);

    const bld = new Uint8Array(0x2500);
    bld[0] = 0x11;
    bld[1] = 0x22;
    bld[2] = 0x33;
    bld[3] = 0x44;
    const hal = new Uint8Array(0x100);
    hal[0] = 0xaa;
    const zImage = new Uint8Array(0x400);
    zImage[0] = 0xde;
    zImage[1] = 0xad;
    zImage[2] = 0xbe;
    zImage[3] = 0xef;
    const initrd = new Uint8Array(0x123);
    initrd[0] = 0x5d;
    initrd[0x122] = 0x77;

    await loader.bootLinuxFlasher({
      bld,
      hal,
      zImage,
      initrd,
      useFastBulk: true,
      skipDelays: true,
    });

    /* Verify fast bulk path ran (stub exec at 0xc0090000, bulk writes, final exec at 0xc0000000) */
    expect(mock.bulkWriteCount).toBeGreaterThan(5);
    expect(mock.execCalls).toEqual([{ addr: 0xc0090000 }, { addr: A5S_BLD_ADDR }]);

    /* Verify loaded images & jump patch in simulated RAM */
    expect(mock.memory.get(A5S_BLD_ADDR)).toBe(0x44332211);
    expect(mock.memory.get(A5S_BLD_JUMP_PATCH_ADDR)).toBe(A5S_BLD_JUMP_PATCH_INSN);
    expect(mock.memory.get(A5S_HAL_ADDR)).toBe(0x000000aa);
    expect(mock.memory.get(A5S_KERNEL_ADDR)).toBe(0xefbeadde);
    expect(mock.readBytes(A5S_INITRD_ADDR, initrd.byteLength)).toEqual(initrd);

    /* Verify ARM trampoline and ATAGS */
    expect(mock.memory.get(A5S_TRAMPOLINE_ADDR + 0x00)).toBe(0xe3a01e40);
    expect(mock.memory.get(A5S_TRAMPOLINE_ADDR + 0x04)).toBe(0xe38110c7);
    expect(mock.memory.get(A5S_TRAMPOLINE_ADDR + 0x10)).toBe(0xea001ffa);
    expect(mock.memory.get(A5S_ATAGS_ADDR + 0x04)).toBe(0x54410001);
    expect(mock.memory.get(A5S_ATAGS_ADDR + 0x0c)).toBe(0x54410009);
    const cmdlineBytes = mock.readBytes(A5S_ATAGS_ADDR + 0x10, 140);
    const cmdlineStr = new TextDecoder().decode(cmdlineBytes).split('\0')[0];
    expect(cmdlineStr).toContain('autoreflash');
    expect(cmdlineStr).toContain('initrd=0xc7000000,0x00000123');
  });

  it('supports legacy EP0 control-transfer fallback when useFastBulk is false', async () => {
    const mock = new MockUsbTransport();
    await mock.open();
    const loader = new AmbarellaLoader(mock);

    const bld = new Uint8Array(0x2500);
    bld[0] = 0x11;
    bld[1] = 0x22;
    bld[2] = 0x33;
    bld[3] = 0x44;
    const hal = new Uint8Array(0x100);
    hal[0] = 0xaa;
    const zImage = new Uint8Array(0x400);
    zImage[0] = 0xde;
    zImage[1] = 0xad;
    zImage[2] = 0xbe;
    zImage[3] = 0xef;
    const initrd = new Uint8Array(0x123);
    initrd[0] = 0x5d;
    initrd[0x122] = 0x77;

    await loader.bootLinuxFlasher({
      bld,
      hal,
      zImage,
      initrd,
      pipelineDepth: 8,
      useFastBulk: false,
      skipDelays: true,
    });

    /* Verify DDR init writes ran in exact order */
    const expectedDdrWrites = HERO2_ALT_DDR_INIT_SEQ.filter((s) => s.kind === 'write');
    expect(mock.regWrites.length).toBe(expectedDdrWrites.length);
    expect(mock.regWrites[0]).toEqual({ addr: 0x70170000, val: 0x09113000 });
    expect(mock.regWrites[mock.regWrites.length - 1]).toEqual({
      addr: 0x60004000,
      val: 0x0000000f,
    });

    /* Verify loaded images & jump patch in simulated RAM */
    expect(mock.bulkWriteCount).toBe(0);
    expect(mock.memory.get(A5S_BLD_ADDR)).toBe(0x44332211);
    expect(mock.memory.get(A5S_BLD_JUMP_PATCH_ADDR)).toBe(A5S_BLD_JUMP_PATCH_INSN);
    expect(mock.memory.get(A5S_HAL_ADDR)).toBe(0x000000aa);
    expect(mock.memory.get(A5S_KERNEL_ADDR)).toBe(0xefbeadde);
    expect(mock.readBytes(A5S_INITRD_ADDR, initrd.byteLength)).toEqual(initrd);

    /* Verify final exec jump */
    expect(mock.execCalls).toEqual([{ addr: A5S_BLD_ADDR }]);
  });
});
