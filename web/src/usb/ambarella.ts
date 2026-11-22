/*
 * TypeScript port of gopro-usb-tools (gp_api.c, gp_ddr.c, gpboot.c) for
 * Ambarella A5s (CAMTYPE_H2 / VID 0x4255 PID 0x0001).
 */

import type { UsbTransport } from './transport';
import {
  crc32,
  FAST_CMD_EXEC,
  FAST_CMD_PING,
  FAST_CMD_WRITE_MEM,
  FAST_CMD_WRITE_WORD,
  FAST_LOADER_ADDR,
  FAST_LOADER_MAGIC_CMD,
  FAST_LOADER_MAGIC_OK,
  FAST_LOADER_STUB,
} from './fastLoaderStub';

export const A5S_DDR_BASE = 0xc0000000;
export const A5S_BLD_ADDR = 0xc0000000;
export const A5S_BLD_JUMP_PATCH_ADDR = 0xc00024c4;
export const A5S_BLD_JUMP_PATCH_INSN = 0xe3a0f4c3; // mov pc, #0xc3000000
export const A5S_HAL_ADDR = 0xc00a0000;
export const A5S_TRAMPOLINE_ADDR = 0xc3000000;
export const A5S_ATAGS_ADDR = 0xc3000020;
export const A5S_KERNEL_ADDR = 0xc3008000;
export const A5S_INITRD_ADDR = 0xc7000000;
export const A5S_MACH_TYPE = 0x04c7;

export const A5S_AUTOREFLASH_CMDLINE =
  'mem=200M@0xc3000000 console=tty0 console=ttyS0,115200n8 root=/dev/ram0 init=/bin/sh autoreflash ';

type DdrStep = { kind: 'write'; addr: number; val: number } | { kind: 'delay' };

/* Exact hero2_alt_ddr_init_seq from gopro-usb-tools/gp_ddr.c (used by CAMTYPE_H2). */
export const HERO2_ALT_DDR_INIT_SEQ: readonly DdrStep[] = [
  { kind: 'write', addr: 0x70170000, val: 0x09113000 },
  { kind: 'write', addr: 0x701700e4, val: 0x13111000 },
  { kind: 'delay' },
  { kind: 'write', addr: 0x70170000, val: 0x09113001 },
  { kind: 'write', addr: 0x701700e4, val: 0x13111001 },
  { kind: 'delay' },
  { kind: 'write', addr: 0x70170068, val: 0x00000003 },
  { kind: 'write', addr: 0x701700dc, val: 0x1b111000 },
  { kind: 'write', addr: 0x70170110, val: 0x3f770000 },
  { kind: 'write', addr: 0x70170110, val: 0x00068300 },
  { kind: 'write', addr: 0x701700dc, val: 0x1b111001 },
  { kind: 'delay' },
  { kind: 'write', addr: 0x701700dc, val: 0x1b111000 },
  { kind: 'write', addr: 0x70170090, val: 0x00204830 },
  { kind: 'write', addr: 0x70170094, val: 0x00204830 },
  { kind: 'write', addr: 0x70170158, val: 0x00007ee5 },
  { kind: 'delay' },
  { kind: 'write', addr: 0x60004004, val: 0x00042175 },
  { kind: 'write', addr: 0x60004008, val: 0x48b7b6a3 },
  { kind: 'write', addr: 0x6000400c, val: 0x01ff3472 },
  { kind: 'write', addr: 0x60004010, val: 0x000000a3 },
  { kind: 'write', addr: 0x60004020, val: 0x00000047 },
  { kind: 'write', addr: 0x60004028, val: 0x00000060 },
  { kind: 'write', addr: 0x60004024, val: 0x00020035 },
  { kind: 'delay' },
  { kind: 'write', addr: 0x60004000, val: 0x00000000 },
  { kind: 'delay' },
  { kind: 'write', addr: 0x60004000, val: 0x0000000c },
  { kind: 'delay' },
  { kind: 'write', addr: 0x60004018, val: 0x01020000 },
  { kind: 'delay' },
  { kind: 'write', addr: 0x60004018, val: 0x01030000 },
  { kind: 'delay' },
  { kind: 'write', addr: 0x60004018, val: 0x01010000 },
  { kind: 'delay' },
  { kind: 'write', addr: 0x60004018, val: 0x01001520 },
  { kind: 'delay' },
  { kind: 'write', addr: 0x60004014, val: 0x00000010 },
  { kind: 'delay' },
  { kind: 'write', addr: 0x60004014, val: 0x00000008 },
  { kind: 'delay' },
  { kind: 'write', addr: 0x60004000, val: 0x0000000e },
  { kind: 'delay' },
  { kind: 'write', addr: 0x60004018, val: 0x01010000 },
  { kind: 'delay' },
  { kind: 'write', addr: 0x60004014, val: 0x00000020 },
  { kind: 'delay' },
  { kind: 'write', addr: 0x60004014, val: 0x00000004 },
  { kind: 'delay' },
  { kind: 'write', addr: 0x60004028, val: 0x00000026 },
  { kind: 'delay' },
  { kind: 'write', addr: 0x60004024, val: 0x00000035 },
  { kind: 'delay' },
  { kind: 'write', addr: 0x60004000, val: 0x0000000f },
  { kind: 'delay' },
];

export interface TransferProgress {
  label: string;
  bytesSent: number;
  bytesTotal: number;
}

export interface BootOptions {
  bld: Uint8Array;
  hal: Uint8Array;
  zImage: Uint8Array;
  initrd: Uint8Array;
  cmdline?: string;
  pipelineDepth?: number;
  useFastBulk?: boolean;
  skipDelays?: boolean;
  onLog?: (msg: string) => void;
  onProgress?: (progress: TransferProgress) => void;
}

function sleep(ms: number): Promise<void> {
  return new Promise((resolve) => setTimeout(resolve, ms));
}

export class AmbarellaLoader {
  private readonly transport: UsbTransport;

  constructor(transport: UsbTransport) {
    this.transport = transport;
  }

  async readReg(addr: number): Promise<number> {
    const uaddr = addr >>> 0;
    if ((uaddr & 3) !== 0) {
      throw new Error(`Unaligned readReg address: 0x${uaddr.toString(16)}`);
    }
    const buf = await this.transport.controlIn(
      0,
      (uaddr >>> 16) & 0xffff,
      uaddr & 0xffff,
      4,
    );
    return (buf[0] | (buf[1] << 8) | (buf[2] << 16) | (buf[3] << 24)) >>> 0;
  }

  async writeReg(addr: number, val: number): Promise<void> {
    const uaddr = addr >>> 0;
    const uval = val >>> 0;
    if ((uaddr & 3) !== 0) {
      throw new Error(`Unaligned writeReg address: 0x${uaddr.toString(16)}`);
    }
    const buf = new Uint8Array(12);
    buf[0] = 0x00;
    buf[1] = 0x00;
    buf[2] = 0x00;
    buf[3] = 0x30;
    buf[4] = uaddr & 0xff;
    buf[5] = (uaddr >>> 8) & 0xff;
    buf[6] = (uaddr >>> 16) & 0xff;
    buf[7] = (uaddr >>> 24) & 0xff;
    buf[8] = uval & 0xff;
    buf[9] = (uval >>> 8) & 0xff;
    buf[10] = (uval >>> 16) & 0xff;
    buf[11] = (uval >>> 24) & 0xff;
    await this.transport.controlOut(0, 0, 0, buf);
  }

  async writeByte(addr: number, val: number): Promise<void> {
    const uaddr = addr >>> 0;
    const wordAddr = (uaddr & ~3) >>> 0;
    const shift = (uaddr & 3) * 8;
    const cur = await this.readReg(wordAddr);
    const next = ((cur & ~(0xff << shift)) | ((val & 0xff) << shift)) >>> 0;
    await this.writeReg(wordAddr, next);
  }

  async writeString(addr: number, str: string): Promise<void> {
    const bytes = new TextEncoder().encode(str);
    for (let i = 0; i < bytes.length; i++) {
      await this.writeByte((addr + i) >>> 0, bytes[i]);
    }
    await this.writeByte((addr + bytes.length) >>> 0, 0);
  }

  async exec(addr: number): Promise<void> {
    const uaddr = addr >>> 0;
    const buf = new Uint8Array(12);
    buf[0] = 0x00;
    buf[1] = 0x00;
    buf[2] = 0x01;
    buf[3] = 0x00;
    buf[4] = uaddr & 0xff;
    buf[5] = (uaddr >>> 8) & 0xff;
    buf[6] = (uaddr >>> 16) & 0xff;
    buf[7] = (uaddr >>> 24) & 0xff;
    await this.transport.controlOut(0, 0, 0, buf);
  }

  /** Quick probe of a readable SoC register to confirm live DFU communication. */
  async probeChip(): Promise<number> {
    return this.readReg(0x70170000);
  }

  private buildFastCmdPacket(op: number, arg1: number, arg2: number): Uint8Array {
    const pkt = new Uint8Array(16);
    const dv = new DataView(pkt.buffer);
    dv.setUint32(0, FAST_LOADER_MAGIC_CMD, true);
    dv.setUint32(4, op >>> 0, true);
    dv.setUint32(8, arg1 >>> 0, true);
    dv.setUint32(12, arg2 >>> 0, true);
    return pkt;
  }

  private async fastCmd(op: number, arg1: number, arg2: number): Promise<[number, number, number]> {
    if (!this.transport.bulkOut || !this.transport.bulkIn) {
      throw new Error('Transport does not support EP1 bulk transfers.');
    }
    await this.transport.bulkOut(1, this.buildFastCmdPacket(op, arg1, arg2));
    const rsp = await this.transport.bulkIn(1, 16);
    if (rsp.byteLength < 16) {
      throw new Error(`Fast loader reply truncated (${rsp.byteLength} B).`);
    }
    const dv = new DataView(rsp.buffer, rsp.byteOffset, rsp.byteLength);
    const magic = dv.getUint32(0, true);
    if (magic !== FAST_LOADER_MAGIC_OK) {
      throw new Error(`Fast loader error reply: magic=0x${magic.toString(16)}`);
    }
    return [dv.getUint32(4, true), dv.getUint32(8, true), dv.getUint32(12, true)];
  }

  private async fastLoadBuffer(
    dstAddr: number,
    data: Uint8Array,
    label: string,
    onProgress?: (sent: number, total: number) => void,
  ): Promise<number> {
    if (!this.transport.bulkOut || !this.transport.bulkIn) {
      throw new Error('Transport does not support EP1 bulk transfers.');
    }
    const hostCrc = crc32(data);
    const total = data.byteLength;
    await this.transport.bulkOut(1, this.buildFastCmdPacket(FAST_CMD_WRITE_MEM, dstAddr, total));

    const chunkSize = 65536;
    let offset = 0;
    while (offset < total) {
      const end = Math.min(total, offset + chunkSize);
      const written = await this.transport.bulkOut(1, data.subarray(offset, end));
      if (written !== end - offset) {
        throw new Error(`EP1 bulkOut short write for ${label}: ${written}/${end - offset}`);
      }
      offset = end;
      onProgress?.(offset, total);
    }

    const rsp = await this.transport.bulkIn(1, 16);
    if (rsp.byteLength < 16) {
      throw new Error(`Fast loader CRC reply truncated for ${label}.`);
    }
    const dv = new DataView(rsp.buffer, rsp.byteOffset, rsp.byteLength);
    const magic = dv.getUint32(0, true);
    const devCrc = dv.getUint32(4, true);
    const devLen = dv.getUint32(8, true);
    if (magic !== FAST_LOADER_MAGIC_OK || devLen !== total || devCrc !== hostCrc) {
      throw new Error(
        `CRC32 verification failed for ${label}: magic=0x${magic.toString(16)} len=${devLen}/${total} crc=0x${devCrc.toString(16)}/0x${hostCrc.toString(16)}`,
      );
    }
    return devCrc;
  }

  private buildTrampolineAndAtagsBuffer(
    cmdline: string,
    initrdAddr: number,
    initrdSize: number,
  ): Uint8Array {
    const fullCmdline =
      initrdSize > 0
        ? `${cmdline} initrd=0x${(initrdAddr >>> 0).toString(16).padStart(8, '0')},0x${(initrdSize >>> 0).toString(16).padStart(8, '0')}`
        : cmdline;
    if (fullCmdline.length >= 256) {
      throw new Error(`Kernel command line too long (${fullCmdline.length} bytes).`);
    }
    const cmdBytes = new TextEncoder().encode(fullCmdline + '\0');
    const cmdTagWords = 2 + Math.floor((fullCmdline.length + 3) / 4);
    const totalLen = 0x20 + (2 + cmdTagWords + 2) * 4;
    const buf = new Uint8Array(totalLen);
    const dv = new DataView(buf.buffer);

    /* ARM trampoline at 0xc3000000 -> branch to zImage at 0xc3008000 */
    dv.setUint32(0x00, (0xe3a01e40 | ((A5S_MACH_TYPE >>> 4) & 0xf0)) >>> 0, true);
    dv.setUint32(0x04, (0xe3811000 | (A5S_MACH_TYPE & 0xff)) >>> 0, true);
    dv.setUint32(0x08, 0xe3a024c3, true);
    dv.setUint32(0x0c, 0xe3822020, true);
    dv.setUint32(0x10, 0xea001ffa, true);

    /* ATAGS at 0xc3000020 */
    dv.setUint32(0x20, 0x00000002, true); // ATAG_CORE size
    dv.setUint32(0x24, 0x54410001, true); // ATAG_CORE
    dv.setUint32(0x28, cmdTagWords, true);
    dv.setUint32(0x2c, 0x54410009, true); // ATAG_CMDLINE
    buf.set(cmdBytes, 0x30);
    return buf;
  }

  async initDdr(skipDelays = false): Promise<void> {
    for (const step of HERO2_ALT_DDR_INIT_SEQ) {
      if (step.kind === 'write') {
        await this.writeReg(step.addr, step.val);
      } else if (!skipDelays) {
        await sleep(10);
      }
    }
  }

  async testDdr(ddrBase = A5S_DDR_BASE, wordCount = 1000): Promise<void> {
    const magic1 = 3126727440;
    const magic2 = 13834356;

    for (let i = 0; i < wordCount; i++) {
      const addr = (ddrBase + i * 4) >>> 0;
      const expect = Math.imul((Math.imul(i, magic2) ^ magic1) >>> 0, i) >>> 0;
      await this.writeReg(addr, expect);
    }

    for (let i = 0; i < wordCount; i++) {
      const addr = (ddrBase + i * 4) >>> 0;
      const expect = Math.imul((Math.imul(i, magic2) ^ magic1) >>> 0, i) >>> 0;
      const got = await this.readReg(addr);
      if (got !== expect) {
        throw new Error(
          `DDR verification failed at 0x${addr.toString(16)}: expected 0x${expect.toString(16)}, got 0x${got.toString(16)}`,
        );
      }
    }
  }

  /**
   * Stream a binary image into Ambarella RAM in 32-bit little-endian words
   * using a true sliding window of `pipelineDepth` concurrent EP0 transfers
   * (no stop-and-wait batch barrier bubbles).
   */
  async loadBuffer(
    baseAddr: number,
    data: Uint8Array,
    label: string,
    opts?: {
      pipelineDepth?: number;
      onProgress?: (sent: number, total: number) => void;
    },
  ): Promise<void> {
    const ubase = baseAddr >>> 0;
    if ((ubase & 3) !== 0) {
      throw new Error(`loadBuffer base address must be 4-byte aligned: 0x${ubase.toString(16)}`);
    }
    const depth = Math.max(1, Math.min(128, Math.trunc(opts?.pipelineDepth ?? 32)));
    const alignedLen = data.byteLength & ~3;
    const total = data.byteLength;
    let nextOffset = 0;
    let completedBytes = 0;
    let lastReport = 0;
    let firstError: unknown = null;

    const worker = async (): Promise<void> => {
      while (firstError === null) {
        const off = nextOffset;
        if (off >= alignedLen) return;
        nextOffset = off + 4;
        const word =
          (data[off] |
            (data[off + 1] << 8) |
            (data[off + 2] << 16) |
            (data[off + 3] << 24)) >>>
          0;
        try {
          await this.writeReg((ubase + off) >>> 0, word);
        } catch (err) {
          if (firstError === null) firstError = err;
          return;
        }
        completedBytes += 4;
        if (
          opts?.onProgress &&
          (completedBytes - lastReport >= 2048 || completedBytes >= alignedLen)
        ) {
          lastReport = completedBytes;
          opts.onProgress(completedBytes, total);
        }
      }
    };

    const workerCount = Math.min(depth, Math.max(1, alignedLen >>> 2));
    const workers: Promise<void>[] = [];
    for (let w = 0; w < workerCount; w++) {
      workers.push(worker());
    }
    await Promise.all(workers);
    if (firstError !== null) {
      throw firstError;
    }

    for (let i = alignedLen; i < total; i++) {
      await this.writeByte((ubase + i) >>> 0, data[i]);
    }
    opts?.onProgress?.(total, total);

    /* Sparse RAM readback verification (start, 25%, 50%, 75%, end words). */
    const sampleOffsets = [
      0,
      Math.floor(alignedLen * 0.25) & ~3,
      Math.floor(alignedLen * 0.5) & ~3,
      Math.floor(alignedLen * 0.75) & ~3,
      Math.max(0, alignedLen - 4),
    ];
    const seen = new Set<number>();
    for (const off of sampleOffsets) {
      if (off + 4 > alignedLen || seen.has(off)) continue;
      seen.add(off);
      const want =
        (data[off] |
          (data[off + 1] << 8) |
          (data[off + 2] << 16) |
          (data[off + 3] << 24)) >>>
        0;
      const got = await this.readReg((ubase + off) >>> 0);
      if (got !== want) {
        throw new Error(
          `RAM verify mismatch for ${label} at +0x${off.toString(16)}: expected 0x${want.toString(16)}, got 0x${got.toString(16)}`,
        );
      }
    }
  }

  async writeAtags(
    cmdline: string,
    initrdAddr: number,
    initrdSize: number,
    tagAddr = A5S_ATAGS_ADDR,
  ): Promise<void> {
    const fullCmdline =
      initrdSize > 0
        ? `${cmdline} initrd=0x${(initrdAddr >>> 0).toString(16).padStart(8, '0')},0x${(initrdSize >>> 0).toString(16).padStart(8, '0')}`
        : cmdline;
    if (fullCmdline.length >= 256) {
      throw new Error(`Kernel command line too long (${fullCmdline.length} bytes).`);
    }
    const cmdTagLen = 2 + Math.floor((fullCmdline.length + 3) / 4);
    await this.writeReg((tagAddr + 0x00) >>> 0, 0x00000002); // ATAG_CORE size
    await this.writeReg((tagAddr + 0x04) >>> 0, 0x54410001); // ATAG_CORE
    await this.writeReg((tagAddr + 0x08) >>> 0, cmdTagLen);
    await this.writeReg((tagAddr + 0x0c) >>> 0, 0x54410009); // ATAG_CMDLINE
    await this.writeString((tagAddr + 0x10) >>> 0, fullCmdline);
    await this.writeReg((tagAddr + 0x08 + cmdTagLen * 4) >>> 0, 0x00000000);
    await this.writeReg((tagAddr + 0x0c + cmdTagLen * 4) >>> 0, 0x00000000);
  }

  async bootLinuxFlasher(opts: BootOptions): Promise<void> {
    const log = opts.onLog ?? (() => {});
    const pipelineDepth = opts.pipelineDepth ?? 32;
    const cmdline = opts.cmdline ?? A5S_AUTOREFLASH_CMDLINE;
    const totalBytes =
      opts.bld.byteLength +
      opts.hal.byteLength +
      opts.zImage.byteLength +
      opts.initrd.byteLength;
    let completedBase = 0;

    const report = (label: string, sent: number) => {
      opts.onProgress?.({
        label,
        bytesSent: completedBase + sent,
        bytesTotal: totalBytes,
      });
    };

    log('Initializing Ambarella A5s DDR memory controller...');
    await this.initDdr(opts.skipDelays ?? false);

    const useFast =
      opts.useFastBulk !== false && Boolean(this.transport.bulkOut && this.transport.bulkIn);
    const testWords = useFast ? 256 : 1000;

    log(`Running DDR read/write integrity test (${testWords} words)...`);
    await this.testDdr(A5S_DDR_BASE, testWords);
    log('DDR test passed.');

    if (useFast) {
      log(
        `Bootstrapping ARMv6 EP1 Bulk-OUT fast loader (${FAST_LOADER_STUB.byteLength} B -> 0x${FAST_LOADER_ADDR.toString(16)})...`,
      );
      await this.loadBuffer(FAST_LOADER_ADDR, FAST_LOADER_STUB, 'fast_loader', {
        pipelineDepth,
      });
      await this.exec(FAST_LOADER_ADDR);
      if (!opts.skipDelays) {
        await sleep(50);
      }
      const [id] = await this.fastCmd(FAST_CMD_PING, 0, 0);
      log(`Fast EP1 Bulk-OUT loader online (id=0x${id.toString(16)}). Streaming images with hardware CRC32...`);

      const bldCrc = await this.fastLoadBuffer(A5S_BLD_ADDR, opts.bld, 'v312-bld.bin', (s) =>
        report('Uploading bootloader (v312-bld.bin)', s),
      );
      completedBase += opts.bld.byteLength;
      log(`  v312-bld.bin (${opts.bld.byteLength.toLocaleString()} B) verified (CRC32=0x${bldCrc.toString(16).padStart(8, '0')}).`);

      const halCrc = await this.fastLoadBuffer(A5S_HAL_ADDR, opts.hal, 'v312-hal-reloc.bin', (s) =>
        report('Uploading HAL (v312-hal-reloc.bin)', s),
      );
      completedBase += opts.hal.byteLength;
      log(`  v312-hal-reloc.bin (${opts.hal.byteLength.toLocaleString()} B) verified (CRC32=0x${halCrc.toString(16).padStart(8, '0')}).`);

      log('Patching BLD jump vector at 0xc00024c4 -> 0xc3000000...');
      await this.fastCmd(FAST_CMD_WRITE_WORD, A5S_BLD_JUMP_PATCH_ADDR, A5S_BLD_JUMP_PATCH_INSN);

      const zCrc = await this.fastLoadBuffer(A5S_KERNEL_ADDR, opts.zImage, 'zImage', (s) =>
        report('Uploading RAM Linux kernel (zImage)', s),
      );
      completedBase += opts.zImage.byteLength;
      log(`  zImage (${opts.zImage.byteLength.toLocaleString()} B) verified (CRC32=0x${zCrc.toString(16).padStart(8, '0')}).`);

      const rdCrc = await this.fastLoadBuffer(A5S_INITRD_ADDR, opts.initrd, 'initrd', (s) =>
        report('Uploading custom flasher initrd', s),
      );
      completedBase += opts.initrd.byteLength;
      log(`  initrd (${opts.initrd.byteLength.toLocaleString()} B) verified (CRC32=0x${rdCrc.toString(16).padStart(8, '0')}).`);

      log(`Writing ARM trampoline (mach_type=0x${A5S_MACH_TYPE.toString(16)}) and ATAGS...`);
      const trampBuf = this.buildTrampolineAndAtagsBuffer(
        cmdline,
        A5S_INITRD_ADDR,
        opts.initrd.byteLength,
      );
      await this.fastLoadBuffer(A5S_TRAMPOLINE_ADDR, trampBuf, 'trampoline+ATAGS');

      log('Executing RAM kernel at 0xc0000000 (autonomous UBIFS installer will run and reboot)...');
      await this.fastCmd(FAST_CMD_EXEC, A5S_BLD_ADDR, 0);
      log('Boot command sent! Camera is now running the autonomous installer in RAM.');
      return;
    }

    log(`Uploading v312-bld.bin (${opts.bld.byteLength.toLocaleString()} B) to 0x${A5S_BLD_ADDR.toString(16)}...`);
    await this.loadBuffer(A5S_BLD_ADDR, opts.bld, 'v312-bld.bin', {
      pipelineDepth,
      onProgress: (s) => report('Uploading bootloader (v312-bld.bin)', s),
    });
    completedBase += opts.bld.byteLength;

    log(`Uploading v312-hal-reloc.bin (${opts.hal.byteLength.toLocaleString()} B) to 0x${A5S_HAL_ADDR.toString(16)}...`);
    await this.loadBuffer(A5S_HAL_ADDR, opts.hal, 'v312-hal-reloc.bin', {
      pipelineDepth,
      onProgress: (s) => report('Uploading HAL (v312-hal-reloc.bin)', s),
    });
    completedBase += opts.hal.byteLength;

    log('Patching BLD jump vector at 0xc00024c4 -> 0xc3000000...');
    await this.writeReg(A5S_BLD_JUMP_PATCH_ADDR, A5S_BLD_JUMP_PATCH_INSN);

    log(`Uploading Linux kernel zImage (${opts.zImage.byteLength.toLocaleString()} B) to 0x${A5S_KERNEL_ADDR.toString(16)}...`);
    await this.loadBuffer(A5S_KERNEL_ADDR, opts.zImage, 'zImage', {
      pipelineDepth,
      onProgress: (s) => report('Uploading RAM Linux kernel (zImage)', s),
    });
    completedBase += opts.zImage.byteLength;

    log(`Uploading custom initramfs (${opts.initrd.byteLength.toLocaleString()} B) to 0x${A5S_INITRD_ADDR.toString(16)}...`);
    await this.loadBuffer(A5S_INITRD_ADDR, opts.initrd, 'initrd', {
      pipelineDepth,
      onProgress: (s) => report('Uploading custom flasher initrd', s),
    });
    completedBase += opts.initrd.byteLength;

    log(`Writing ARM trampoline (mach_type=0x${A5S_MACH_TYPE.toString(16)}) and ATAGS...`);
    await this.writeReg(
      A5S_TRAMPOLINE_ADDR + 0x00,
      (0xe3a01e40 | ((A5S_MACH_TYPE >>> 4) & 0xf0)) >>> 0,
    );
    await this.writeReg(
      A5S_TRAMPOLINE_ADDR + 0x04,
      (0xe3811000 | (A5S_MACH_TYPE & 0xff)) >>> 0,
    );
    await this.writeReg(A5S_TRAMPOLINE_ADDR + 0x08, 0xe3a024c3);
    await this.writeReg(A5S_TRAMPOLINE_ADDR + 0x0c, 0xe3822020);
    await this.writeReg(A5S_TRAMPOLINE_ADDR + 0x10, 0xea001ffa);

    await this.writeAtags(cmdline, A5S_INITRD_ADDR, opts.initrd.byteLength, A5S_ATAGS_ADDR);

    log('Executing RAM kernel at 0xc0000000 (autonomous UBIFS installer will run and reboot)...');
    await this.exec(A5S_BLD_ADDR);
    log('Boot command sent! Camera is now running the autonomous installer in RAM.');
  }
}
