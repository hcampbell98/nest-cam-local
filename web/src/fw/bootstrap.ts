export interface PatchEntry {
  readonly offset: number;
  readonly val: number;
}

export const HD2_FIRMWARE_SIZE = 52_850_688;
export const HD2_FIRMWARE_SHA256 =
  '8605b300afbd1452493e2fccd7d852e1e1fac7953266986ef095960fe700e406';

export const V312_BLD_START = 8448;
export const V312_BLD_SIZE = 163_216;
export const V312_BLD_SHA256 =
  '1c5a1b9158c139197d16ce62e159b58f5c6ae5f3944845177db044877e34a581';

export const V312_HAL_START = 172_288;
export const V312_HAL_SIZE = 55_724;
export const V312_HAL_SHA256 =
  '82e475f0d01529d1ce95e356138959c3222d3317bb56d7e67683ebe9c98a03aa';

/* Exact 141-byte patch table from gopro-usb-tools/fw-patch.h (hal_patch_v312). */
export const HAL_PATCH_V312: readonly PatchEntry[] = [
  { offset: 0x000c, val: 0xac },
  { offset: 0x000d, val: 0xd9 },
  { offset: 0x8007, val: 0xd9 },
  { offset: 0x800c, val: 0x01 },
  { offset: 0x8043, val: 0xd9 },
  { offset: 0x8047, val: 0xd8 },
  { offset: 0x804b, val: 0xd8 },
  { offset: 0x804f, val: 0xd9 },
  { offset: 0x8053, val: 0xd9 },
  { offset: 0x8057, val: 0xd9 },
  { offset: 0x805f, val: 0xd9 },
  { offset: 0xc871, val: 0x36 },
  { offset: 0xc872, val: 0x6e },
  { offset: 0xc873, val: 0x01 },
  { offset: 0xc88d, val: 0x36 },
  { offset: 0xc88e, val: 0x6e },
  { offset: 0xc88f, val: 0x01 },
  { offset: 0xc8a8, val: 0x01 },
  { offset: 0xc8a9, val: 0x8d },
  { offset: 0xc8aa, val: 0x6d },
  { offset: 0xc8ab, val: 0x01 },
  { offset: 0xc8bc, val: 0xff },
  { offset: 0xc8bd, val: 0xff },
  { offset: 0xc8c5, val: 0xd8 },
  { offset: 0xc8c6, val: 0xb8 },
  { offset: 0xc8c7, val: 0x05 },
  { offset: 0xc8e1, val: 0xf4 },
  { offset: 0xc8e2, val: 0x06 },
  { offset: 0xc8e3, val: 0x14 },
  { offset: 0xc8fd, val: 0xe8 },
  { offset: 0xc8fe, val: 0x0d },
  { offset: 0xc8ff, val: 0x28 },
  { offset: 0xc919, val: 0x0e },
  { offset: 0xc91a, val: 0x27 },
  { offset: 0xc91b, val: 0x07 },
  { offset: 0xc935, val: 0x87 },
  { offset: 0xc936, val: 0x93 },
  { offset: 0xc937, val: 0x03 },
  { offset: 0xc951, val: 0x1c },
  { offset: 0xc952, val: 0x4e },
  { offset: 0xc953, val: 0x0e },
  { offset: 0xc96d, val: 0x80 },
  { offset: 0xc96e, val: 0xbb },
  { offset: 0xcb41, val: 0x36 },
  { offset: 0xcb42, val: 0x6e },
  { offset: 0xcb43, val: 0x01 },
  { offset: 0xcb44, val: 0x7f },
  { offset: 0xcb45, val: 0x33 },
  { offset: 0xcb48, val: 0xc8 },
  { offset: 0xcb49, val: 0x32 },
  { offset: 0xcb4c, val: 0x60 },
  { offset: 0xcb4d, val: 0xfe },
  { offset: 0xcb4e, val: 0xcd },
  { offset: 0xcb50, val: 0x60 },
  { offset: 0xcb51, val: 0xfe },
  { offset: 0xcb52, val: 0xcd },
  { offset: 0xcb54, val: 0x7a },
  { offset: 0xcb55, val: 0x69 },
  { offset: 0xcb58, val: 0x80 },
  { offset: 0xcb59, val: 0x8d },
  { offset: 0xcb5a, val: 0x5b },
  { offset: 0xcb5c, val: 0xe0 },
  { offset: 0xcb5d, val: 0x70 },
  { offset: 0xcb5e, val: 0x72 },
  { offset: 0xcb61, val: 0x36 },
  { offset: 0xcb62, val: 0x6e },
  { offset: 0xcb63, val: 0x01 },
  { offset: 0xcb64, val: 0x62 },
  { offset: 0xcb65, val: 0xb3 },
  { offset: 0xcb66, val: 0x19 },
  { offset: 0xcb67, val: 0x01 },
  { offset: 0xcb68, val: 0xe8 },
  { offset: 0xcb69, val: 0x03 },
  { offset: 0xcb6f, val: 0xd9 },
  { offset: 0xcb73, val: 0xd8 },
  { offset: 0xd8cc, val: 0x03 },
  { offset: 0xd8d0, val: 0x04 },
  { offset: 0xd8d4, val: 0x01 },
  { offset: 0xd8d8, val: 0x01 },
  { offset: 0xd8dc, val: 0x01 },
  { offset: 0xd8e7, val: 0xd8 },
  { offset: 0xd8ee, val: 0xe0 },
  { offset: 0xd8ef, val: 0xfe },
  { offset: 0xd8f2, val: 0xe0 },
  { offset: 0xd8f3, val: 0xfe },
  { offset: 0xd8f6, val: 0xe0 },
  { offset: 0xd8f7, val: 0xfe },
  { offset: 0xd8fa, val: 0xe0 },
  { offset: 0xd8fb, val: 0xfe },
  { offset: 0xd8fe, val: 0xe0 },
  { offset: 0xd8ff, val: 0xfe },
  { offset: 0xd902, val: 0xe0 },
  { offset: 0xd903, val: 0xfe },
  { offset: 0xd906, val: 0xe0 },
  { offset: 0xd907, val: 0xfe },
  { offset: 0xd90a, val: 0xe0 },
  { offset: 0xd90b, val: 0xfe },
  { offset: 0xd90e, val: 0xe0 },
  { offset: 0xd90f, val: 0xfe },
  { offset: 0xd912, val: 0xe0 },
  { offset: 0xd913, val: 0xfe },
  { offset: 0xd916, val: 0xe0 },
  { offset: 0xd917, val: 0xfe },
  { offset: 0xd91a, val: 0xe0 },
  { offset: 0xd91b, val: 0xfe },
  { offset: 0xd91e, val: 0xe0 },
  { offset: 0xd91f, val: 0xfe },
  { offset: 0xd922, val: 0xe0 },
  { offset: 0xd923, val: 0xfe },
  { offset: 0xd926, val: 0xe0 },
  { offset: 0xd927, val: 0xfe },
  { offset: 0xd92a, val: 0xe0 },
  { offset: 0xd92b, val: 0xfe },
  { offset: 0xd92e, val: 0xe0 },
  { offset: 0xd92f, val: 0xfe },
  { offset: 0xd932, val: 0xe0 },
  { offset: 0xd933, val: 0xfe },
  { offset: 0xd936, val: 0xe0 },
  { offset: 0xd937, val: 0xfe },
  { offset: 0xd93a, val: 0xe0 },
  { offset: 0xd93b, val: 0xfe },
  { offset: 0xd93e, val: 0xe0 },
  { offset: 0xd93f, val: 0xfe },
  { offset: 0xd942, val: 0xe0 },
  { offset: 0xd943, val: 0xfe },
  { offset: 0xd946, val: 0xe0 },
  { offset: 0xd947, val: 0xfe },
  { offset: 0xd94a, val: 0xe0 },
  { offset: 0xd94b, val: 0xfe },
  { offset: 0xd94e, val: 0xe0 },
  { offset: 0xd94f, val: 0xfe },
  { offset: 0xd952, val: 0xe0 },
  { offset: 0xd953, val: 0xfe },
  { offset: 0xd956, val: 0xe0 },
  { offset: 0xd957, val: 0xfe },
  { offset: 0xd95a, val: 0xe0 },
  { offset: 0xd95b, val: 0xfe },
  { offset: 0xd95e, val: 0xe0 },
  { offset: 0xd95f, val: 0xfe },
  { offset: 0xd962, val: 0xe0 },
  { offset: 0xd963, val: 0xfe },
];

export interface BootstrapImages {
  bld: Uint8Array;
  hal: Uint8Array;
  firmwareSha256: string;
  bldSha256: string;
  halSha256: string;
}

export async function sha256Hex(data: Uint8Array): Promise<string> {
  const copy = new Uint8Array(data.byteLength);
  copy.set(data);
  const digest = await crypto.subtle.digest('SHA-256', copy.buffer);
  const bytes = new Uint8Array(digest);
  let hex = '';
  for (let i = 0; i < bytes.length; i++) {
    hex += bytes[i].toString(16).padStart(2, '0');
  }
  return hex;
}

export async function extractBootstrapFromHd2(
  firmwareBytes: Uint8Array,
): Promise<BootstrapImages> {
  if (firmwareBytes.byteLength !== HD2_FIRMWARE_SIZE) {
    throw new Error(
      `Invalid HD2-firmware.bin size: expected ${HD2_FIRMWARE_SIZE.toLocaleString()} bytes, got ${firmwareBytes.byteLength.toLocaleString()} bytes.`,
    );
  }

  const firmwareSha256 = await sha256Hex(firmwareBytes);
  if (firmwareSha256 !== HD2_FIRMWARE_SHA256) {
    throw new Error(
      `Unexpected HD2-firmware.bin SHA-256 (${firmwareSha256.slice(0, 12)}...). Please supply the Hero2 v312 HD2-firmware.bin file.`,
    );
  }

  const bld = firmwareBytes.slice(V312_BLD_START, V312_BLD_START + V312_BLD_SIZE);
  const hal = firmwareBytes.slice(V312_HAL_START, V312_HAL_START + V312_HAL_SIZE);

  for (const patch of HAL_PATCH_V312) {
    hal[patch.offset] = patch.val;
  }

  const [bldSha256, halSha256] = await Promise.all([sha256Hex(bld), sha256Hex(hal)]);
  if (bldSha256 !== V312_BLD_SHA256 || halSha256 !== V312_HAL_SHA256) {
    throw new Error('Extracted bootstrap checksum verification failed.');
  }

  return { bld, hal, firmwareSha256, bldSha256, halSha256 };
}
