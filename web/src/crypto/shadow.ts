/*
 * Pure TypeScript implementation of glibc SHA-512-crypt ($6$) password hashing
 * (Ulrich Drepper specification, 5000 rounds), compatible with glibc 2.13's
 * libcrypt-2.13.so (__sha512_crypt_r) on the Gen 1 Nest Cam Outdoor.
 */

const CRYPT_B64 = './0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz';
const DEFAULT_ROUNDS = 5000;

async function sha512(chunks: readonly Uint8Array[]): Promise<Uint8Array> {
  let total = 0;
  for (const c of chunks) total += c.byteLength;
  const buf = new Uint8Array(total);
  let off = 0;
  for (const c of chunks) {
    buf.set(c, off);
    off += c.byteLength;
  }
  const digest = await crypto.subtle.digest('SHA-512', buf.buffer);
  return new Uint8Array(digest);
}

function repeatToLength(src: Uint8Array, targetLen: number): Uint8Array {
  const out = new Uint8Array(targetLen);
  let off = 0;
  while (off < targetLen) {
    const take = Math.min(src.byteLength, targetLen - off);
    out.set(src.subarray(0, take), off);
    off += take;
  }
  return out;
}

function encodeBase64Triplet(b2: number, b1: number, b0: number, n: number): string {
  let w = (b2 << 16) | (b1 << 8) | b0;
  let out = '';
  for (let i = 0; i < n; i++) {
    out += CRYPT_B64[w & 0x3f];
    w >>>= 6;
  }
  return out;
}

export function generateCryptSalt(length = 16): string {
  const bytes = new Uint8Array(length);
  crypto.getRandomValues(bytes);
  let out = '';
  for (let i = 0; i < length; i++) {
    out += CRYPT_B64[bytes[i] % CRYPT_B64.length];
  }
  return out;
}

export async function sha512Crypt(
  password: string,
  saltInput?: string,
  rounds = DEFAULT_ROUNDS,
): Promise<string> {
  const enc = new TextEncoder();
  const key = enc.encode(password);
  const rawSalt = (saltInput ?? generateCryptSalt(16)).slice(0, 16);
  if (!/^[./0-9A-Za-z]{1,16}$/.test(rawSalt)) {
    throw new Error('Invalid crypt salt characters.');
  }
  const salt = enc.encode(rawSalt);

  /* Step 4-8: Digest B = SHA512(key + salt + key) */
  const digestB = await sha512([key, salt, key]);

  /* Step 1-12: Digest A */
  const chunksA: Uint8Array[] = [key, salt];
  for (let rem = key.byteLength; rem > 0; rem -= 64) {
    chunksA.push(rem > 64 ? digestB : digestB.subarray(0, rem));
  }
  for (let cnt = key.byteLength; cnt > 0; cnt >>>= 1) {
    chunksA.push(cnt & 1 ? digestB : key);
  }
  let digestC = await sha512(chunksA);

  /* Step 13-16: Digest DP -> byte sequence P of length key.length */
  const chunksDP: Uint8Array[] = [];
  for (let i = 0; i < key.byteLength; i++) chunksDP.push(key);
  const digestDP = await sha512(chunksDP);
  const seqP = repeatToLength(digestDP, key.byteLength);

  /* Step 17-20: Digest DS -> byte sequence S of length salt.length */
  const chunksDS: Uint8Array[] = [];
  const sRepeats = 16 + digestC[0];
  for (let i = 0; i < sRepeats; i++) chunksDS.push(salt);
  const digestDS = await sha512(chunksDS);
  const seqS = repeatToLength(digestDS, salt.byteLength);

  /* Step 21: 5000 rounds */
  const clampedRounds = Math.max(1000, Math.min(999_999_999, Math.trunc(rounds)));
  for (let i = 0; i < clampedRounds; i++) {
    const parts: Uint8Array[] = [];
    if (i & 1) parts.push(seqP);
    else parts.push(digestC);
    if (i % 3 !== 0) parts.push(seqS);
    if (i % 7 !== 0) parts.push(seqP);
    if (i & 1) parts.push(digestC);
    else parts.push(seqP);
    digestC = await sha512(parts);
  }

  /* Step 22: Custom permutation + base64 encoding */
  const perm: ReadonlyArray<[number, number, number]> = [
    [0, 21, 42],
    [22, 43, 1],
    [44, 2, 23],
    [3, 24, 45],
    [25, 46, 4],
    [47, 5, 26],
    [6, 27, 48],
    [28, 49, 7],
    [50, 8, 29],
    [9, 30, 51],
    [31, 52, 10],
    [53, 11, 32],
    [12, 33, 54],
    [34, 55, 13],
    [56, 14, 35],
    [15, 36, 57],
    [37, 58, 16],
    [59, 17, 38],
    [18, 39, 60],
    [40, 61, 19],
    [62, 20, 41],
  ];

  let b64 = '';
  for (const [i2, i1, i0] of perm) {
    b64 += encodeBase64Triplet(digestC[i2], digestC[i1], digestC[i0], 4);
  }
  b64 += encodeBase64Triplet(0, 0, digestC[63], 2);

  const prefix = clampedRounds === DEFAULT_ROUNDS ? '$6$' : `$6$rounds=${clampedRounds}$`;
  return `${prefix}${rawSalt}$${b64}`;
}

export async function generateShadowFile(rootPassword: string): Promise<string> {
  if (!rootPassword || rootPassword.length < 4) {
    throw new Error('Root SSH password must be at least 4 characters.');
  }
  const hash = await sha512Crypt(rootPassword);
  return [
    `root:${hash}:19000:0:99999:7:::`,
    'bin:*:10933:0:99999:7:::',
    'daemon:*:10933:0:99999:7:::',
    'adm:*:10933:0:99999:7:::',
    'lp:*:10933:0:99999:7:::',
    'sync:*:10933:0:99999:7:::',
    'shutdown:*:10933:0:99999:7:::',
    'halt:*:10933:0:99999:7:::',
    'uucp:*:10933:0:99999:7:::',
    'operator:*:10933:0:99999:7:::',
    'nobody:*:10933:0:99999:7:::',
    'default:*:10933:0:99999:7:::',
    '',
  ].join('\n');
}
