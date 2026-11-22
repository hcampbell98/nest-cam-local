/*
 * POSIX SVR4 "newc" (070701) cpio archive builder, merger, and gzip initramfs
 * packer for Linux 2.6.38.8. Produces a single standard gzip-compressed cpio
 * archive (avoiding multi-segment LZMA+gzip boundary quirks on older kernels).
 */

export interface CpioEntry {
  /** Path inside initramfs without leading slash (e.g. "usr/bin/autoreflash.sh") */
  path: string;
  /** Octal POSIX mode (e.g. 0o100755 for executable file, 0o040755 for dir) */
  mode: number;
  /** File content (omit or empty for directories) */
  data?: Uint8Array;
  mtime?: number;
}

const ENC = new TextEncoder();
const DEC = new TextDecoder('latin1');

function hex8(n: number): string {
  return (n >>> 0).toString(16).padStart(8, '0');
}

function pad4(n: number): number {
  return (4 - (n & 3)) & 3;
}

function align4(n: number): number {
  return (n + 3) & ~3;
}

function normalizePath(p: string): string {
  return p.replace(/^(\.\/)+/, '').replace(/^\/+/, '').replace(/\/+$/, '');
}

function encodeNewcRecord(
  ino: number,
  name: string,
  mode: number,
  data: Uint8Array,
  nlink: number,
  mtime: number,
): Uint8Array {
  const nameBytes = ENC.encode(name + '\0');
  const headerStr =
    '070701' +
    hex8(ino) +
    hex8(mode) +
    hex8(0) + // uid
    hex8(0) + // gid
    hex8(nlink) +
    hex8(mtime) +
    hex8(data.byteLength) +
    hex8(0) + // devmajor
    hex8(0) + // devminor
    hex8(0) + // rdevmajor
    hex8(0) + // rdevminor
    hex8(nameBytes.byteLength) +
    hex8(0); // check
  const headerBytes = ENC.encode(headerStr);
  const nameTotal = align4(headerBytes.byteLength + nameBytes.byteLength);
  const dataTotal = align4(data.byteLength);
  const out = new Uint8Array(nameTotal + dataTotal);
  out.set(headerBytes, 0);
  out.set(nameBytes, headerBytes.byteLength);
  if (data.byteLength > 0) {
    out.set(data, nameTotal);
  }
  return out;
}

function concatArrays(chunks: readonly Uint8Array[]): Uint8Array {
  let total = 0;
  for (const c of chunks) total += c.byteLength;
  const out = new Uint8Array(total);
  let off = 0;
  for (const c of chunks) {
    out.set(c, off);
    off += c.byteLength;
  }
  return out;
}

export function buildCpioNewc(entries: readonly CpioEntry[]): Uint8Array {
  const chunks: Uint8Array[] = [];
  let ino = 100_000;
  const defaultMtime = 1_700_000_000;

  for (const entry of entries) {
    const clean = normalizePath(entry.path);
    if (!clean) continue;
    const isDir = (entry.mode & 0o170000) === 0o040000;
    const body = isDir ? new Uint8Array(0) : (entry.data ?? new Uint8Array(0));
    chunks.push(
      encodeNewcRecord(ino++, clean, entry.mode, body, isDir ? 2 : 1, entry.mtime ?? defaultMtime),
    );
  }

  chunks.push(encodeNewcRecord(0, 'TRAILER!!!', 0, new Uint8Array(0), 1, 0));
  return concatArrays(chunks);
}

/**
 * Merge overlay entries into an existing SVR4 newc (070701) cpio archive:
 * keeps existing directories in their original order (so Linux 2.6.38 initramfs
 * unpack never sees child files before parent directories), replaces matching
 * non-directory paths, and appends new overlay entries plus a single TRAILER!!!.
 */
export function mergeCpioNewc(
  baseCpio: Uint8Array,
  overlayEntries: readonly CpioEntry[],
): Uint8Array {
  const replaceFiles = new Set<string>();
  for (const e of overlayEntries) {
    const clean = normalizePath(e.path);
    const isDir = (e.mode & 0o170000) === 0o040000;
    if (clean && !isDir) replaceFiles.add(clean);
  }

  const existingDirs = new Set<string>();
  const chunks: Uint8Array[] = [];
  let pos = 0;
  let maxIno = 100_000;
  let foundTrailer = false;

  while (pos + 110 <= baseCpio.byteLength) {
    const magic = DEC.decode(baseCpio.subarray(pos, pos + 6));
    if (magic !== '070701' && magic !== '070702') {
      throw new Error(`Invalid base cpio magic "${magic}" at offset ${pos}.`);
    }
    const ino = parseInt(DEC.decode(baseCpio.subarray(pos + 6, pos + 14)), 16);
    const mode = parseInt(DEC.decode(baseCpio.subarray(pos + 14, pos + 22)), 16);
    const fileSize = parseInt(DEC.decode(baseCpio.subarray(pos + 54, pos + 62)), 16);
    const nameSize = parseInt(DEC.decode(baseCpio.subarray(pos + 94, pos + 102)), 16);
    if (Number.isNaN(fileSize) || Number.isNaN(nameSize) || nameSize < 1) {
      throw new Error(`Malformed base cpio header at offset ${pos}.`);
    }

    const nameEnd = pos + 110 + nameSize;
    const dataStart = align4(nameEnd);
    const recordEnd = align4(dataStart + fileSize);
    if (recordEnd > baseCpio.byteLength) {
      throw new Error(`Truncated base cpio record at offset ${pos}.`);
    }

    const rawName = DEC.decode(baseCpio.subarray(pos + 110, nameEnd - 1));
    if (rawName === 'TRAILER!!!') {
      foundTrailer = true;
      break;
    }

    if (ino >= maxIno) maxIno = ino + 1;
    const clean = normalizePath(rawName);
    const isBaseDir = (mode & 0o170000) === 0o040000;
    if (isBaseDir && clean) {
      existingDirs.add(clean);
    }
    if (isBaseDir || !replaceFiles.has(clean)) {
      chunks.push(baseCpio.subarray(pos, recordEnd));
    }
    pos = recordEnd;
  }

  if (!foundTrailer) {
    throw new Error('Base cpio archive did not contain a TRAILER!!! record.');
  }

  const defaultMtime = 1_700_000_000;
  for (const entry of overlayEntries) {
    const clean = normalizePath(entry.path);
    if (!clean) continue;
    const isDir = (entry.mode & 0o170000) === 0o040000;
    if (isDir && existingDirs.has(clean)) continue;
    const body = isDir ? new Uint8Array(0) : (entry.data ?? new Uint8Array(0));
    chunks.push(
      encodeNewcRecord(
        maxIno++,
        clean,
        entry.mode,
        body,
        isDir ? 2 : 1,
        entry.mtime ?? defaultMtime,
      ),
    );
  }

  chunks.push(encodeNewcRecord(0, 'TRAILER!!!', 0, new Uint8Array(0), 1, 0));
  return concatArrays(chunks);
}

async function runStream(
  data: Uint8Array,
  transform: {
    writable: WritableStream<BufferSource>;
    readable: ReadableStream<Uint8Array>;
  },
): Promise<Uint8Array> {
  const writer = transform.writable.getWriter();
  const copy = new Uint8Array(data.byteLength);
  copy.set(data);
  const writeDone = writer.write(copy).then(() => writer.close());

  const reader = transform.readable.getReader();
  const parts: Uint8Array[] = [];
  while (true) {
    const { value, done } = await reader.read();
    if (done) break;
    parts.push(value);
  }
  await writeDone;
  return concatArrays(parts);
}

export async function gzipCompress(data: Uint8Array): Promise<Uint8Array> {
  return runStream(data, new CompressionStream('gzip'));
}

export async function gzipDecompress(data: Uint8Array): Promise<Uint8Array> {
  return runStream(data, new DecompressionStream('gzip'));
}

/**
 * Unpack the base gzipped cpio initramfs, merge overlay entries cleanly with no
 * duplicate paths, and re-compress into one standard initrd.cpio.gz image.
 */
export async function buildMergedInitrd(
  baseInitrdData: Uint8Array,
  overlayEntries: readonly CpioEntry[],
): Promise<{
  initrd: Uint8Array;
  baseCpioSize: number;
  mergedCpioSize: number;
  compressedSize: number;
}> {
  const isGzip =
    baseInitrdData.byteLength >= 2 &&
    baseInitrdData[0] === 0x1f &&
    baseInitrdData[1] === 0x8b;
  const baseCpio = isGzip ? await gzipDecompress(baseInitrdData) : baseInitrdData;
  const mergedCpio = mergeCpioNewc(baseCpio, overlayEntries);
  const initrd = await gzipCompress(mergedCpio);
  const pad = pad4(initrd.byteLength);
  const padded = pad > 0 ? concatArrays([initrd, new Uint8Array(pad)]) : initrd;

  return {
    initrd: padded,
    baseCpioSize: baseCpio.byteLength,
    mergedCpioSize: mergedCpio.byteLength,
    compressedSize: padded.byteLength,
  };
}
