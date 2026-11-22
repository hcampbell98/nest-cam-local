import {
  crc32,
  FAST_CMD_EXEC,
  FAST_CMD_PING,
  FAST_CMD_READ_WORD,
  FAST_CMD_WRITE_MEM,
  FAST_CMD_WRITE_WORD,
  FAST_LOADER_ADDR,
  FAST_LOADER_MAGIC_CMD,
  FAST_LOADER_MAGIC_OK,
} from './fastLoaderStub';

export const AMBARELLA_VID = 0x4255;
export const AMBARELLA_A5S_PID = 0x0001;

export interface EndpointSummary {
  endpointNumber: number;
  direction: 'in' | 'out';
  type: 'bulk' | 'interrupt' | 'isochronous';
  packetSize: number;
}

export interface InterfaceSummary {
  interfaceNumber: number;
  alternateSetting: number;
  interfaceClass: number;
  interfaceSubclass: number;
  interfaceProtocol: number;
  endpoints: EndpointSummary[];
}

export interface UsbDescriptorReport {
  usbVersion: string;
  deviceClass: number;
  deviceSubclass: number;
  deviceProtocol: number;
  activeConfiguration: number | null;
  configurations: Array<{
    configurationValue: number;
    interfaces: InterfaceSummary[];
  }>;
  bulkOutEndpoints: EndpointSummary[];
  bulkInEndpoints: EndpointSummary[];
}

export interface DeviceInfo {
  vendorId: number;
  productId: number;
  productName: string;
  manufacturerName: string;
  simulated: boolean;
  descriptors?: UsbDescriptorReport;
}

export interface UsbTransport {
  open(): Promise<void>;
  close(): Promise<void>;
  getDeviceInfo(): DeviceInfo;
  controlIn(
    request: number,
    value: number,
    index: number,
    length: number,
  ): Promise<Uint8Array>;
  controlOut(
    request: number,
    value: number,
    index: number,
    data: Uint8Array,
  ): Promise<void>;
  bulkOut?(endpointNumber: number, data: Uint8Array): Promise<number>;
  bulkIn?(endpointNumber: number, length: number): Promise<Uint8Array>;
  clearHalt?(direction: 'in' | 'out', endpointNumber: number): Promise<void>;
}

export function isWebUsbSupported(): boolean {
  return typeof navigator !== 'undefined' && 'usb' in navigator && !!navigator.usb;
}

export async function requestAmbarellaDevice(): Promise<USBDevice> {
  if (!isWebUsbSupported()) {
    throw new Error(
      'WebUSB is not supported in this browser. Please use Chrome, Edge, or Brave on desktop.',
    );
  }
  return navigator.usb.requestDevice({
    filters: [{ vendorId: AMBARELLA_VID }],
  });
}

export class WebUsbTransport implements UsbTransport {
  private readonly device: USBDevice;
  private claimedInterface: number | null = null;

  constructor(device: USBDevice) {
    this.device = device;
  }

  getDescriptorReport(): UsbDescriptorReport {
    const bulkOutEndpoints: EndpointSummary[] = [];
    const bulkInEndpoints: EndpointSummary[] = [];
    const configurations = this.device.configurations.map((cfg) => {
      const interfaces: InterfaceSummary[] = [];
      for (const iface of cfg.interfaces) {
        for (const alt of iface.alternates) {
          const endpoints: EndpointSummary[] = alt.endpoints.map((ep) => ({
            endpointNumber: ep.endpointNumber,
            direction: ep.direction,
            type: ep.type,
            packetSize: ep.packetSize,
          }));
          for (const ep of endpoints) {
            if (ep.type === 'bulk' && ep.direction === 'out') bulkOutEndpoints.push(ep);
            if (ep.type === 'bulk' && ep.direction === 'in') bulkInEndpoints.push(ep);
          }
          interfaces.push({
            interfaceNumber: iface.interfaceNumber,
            alternateSetting: alt.alternateSetting,
            interfaceClass: alt.interfaceClass,
            interfaceSubclass: alt.interfaceSubclass,
            interfaceProtocol: alt.interfaceProtocol,
            endpoints,
          });
        }
      }
      return {
        configurationValue: cfg.configurationValue,
        interfaces,
      };
    });

    return {
      usbVersion: `${this.device.usbVersionMajor}.${this.device.usbVersionMinor}.${this.device.usbVersionSubminor}`,
      deviceClass: this.device.deviceClass,
      deviceSubclass: this.device.deviceSubclass,
      deviceProtocol: this.device.deviceProtocol,
      activeConfiguration: this.device.configuration?.configurationValue ?? null,
      configurations,
      bulkOutEndpoints,
      bulkInEndpoints,
    };
  }

  getDeviceInfo(): DeviceInfo {
    return {
      vendorId: this.device.vendorId,
      productId: this.device.productId,
      productName: this.device.productName || 'Ambarella A5s USB Bootloader',
      manufacturerName: this.device.manufacturerName || 'Ambarella',
      simulated: false,
      descriptors: this.getDescriptorReport(),
    };
  }

  async open(): Promise<void> {
    if (!this.device.opened) {
      await this.device.open();
    }
    /*
     * WebUSB does not expose an unconfigured "configuration 0" state; only
     * select configuration 1 when the device is not yet configured.
     */
    if (this.device.configuration === null && this.device.configurations.length > 0) {
      const cfgNum = this.device.configurations[0].configurationValue;
      if (cfgNum > 0) {
        try {
          await this.device.selectConfiguration(cfgNum);
        } catch {
          /* Some Ambarella DFU stacks continue fine without explicit selectConfiguration. */
        }
      }
    }

    /*
     * Device-recipient control transfers do not strictly require claiming an
     * interface in the WebUSB spec, but Windows WinUSB requires an interface
     * handle to route EP0 transfers reliably.
     */
    const ifaces = this.device.configuration?.interfaces ?? [];
    if (ifaces.length > 0) {
      const ifaceNum = ifaces[0].interfaceNumber;
      try {
        await this.device.claimInterface(ifaceNum);
        this.claimedInterface = ifaceNum;
      } catch {
        this.claimedInterface = null;
      }
    }
  }

  async close(): Promise<void> {
    if (!this.device.opened) return;
    if (this.claimedInterface !== null) {
      try {
        await this.device.releaseInterface(this.claimedInterface);
      } catch {
        /* Ignore release errors on disconnect/exec. */
      }
      this.claimedInterface = null;
    }
    try {
      await this.device.close();
    } catch {
      /* Ignore close errors after exec reboot. */
    }
  }

  async controlIn(
    request: number,
    value: number,
    index: number,
    length: number,
  ): Promise<Uint8Array> {
    const res = await this.device.controlTransferIn(
      {
        requestType: 'vendor',
        recipient: 'device',
        request,
        value,
        index,
      },
      length,
    );
    if (res.status !== 'ok' || !res.data || res.data.byteLength !== length) {
      throw new Error(
        `USB controlIn failed (status=${res.status}, got=${res.data?.byteLength ?? 0}, want=${length})`,
      );
    }
    return new Uint8Array(res.data.buffer, res.data.byteOffset, res.data.byteLength);
  }

  async controlOut(
    request: number,
    value: number,
    index: number,
    data: Uint8Array,
  ): Promise<void> {
    const copy = new Uint8Array(data.byteLength);
    copy.set(data);
    const res = await this.device.controlTransferOut(
      {
        requestType: 'vendor',
        recipient: 'device',
        request,
        value,
        index,
      },
      copy.buffer,
    );
    if (res.status !== 'ok' || res.bytesWritten !== data.byteLength) {
      throw new Error(
        `USB controlOut failed (status=${res.status}, written=${res.bytesWritten}, want=${data.byteLength})`,
      );
    }
  }

  async bulkOut(endpointNumber: number, data: Uint8Array): Promise<number> {
    const copy = new Uint8Array(data.byteLength);
    copy.set(data);
    const res = await this.device.transferOut(endpointNumber, copy.buffer);
    if (res.status !== 'ok') {
      throw new Error(`USB bulkOut(EP${endpointNumber}) failed (status=${res.status})`);
    }
    return res.bytesWritten;
  }

  async bulkIn(endpointNumber: number, length: number): Promise<Uint8Array> {
    const res = await this.device.transferIn(endpointNumber, length);
    if (res.status !== 'ok' || !res.data) {
      throw new Error(`USB bulkIn(EP${endpointNumber}) failed (status=${res.status})`);
    }
    return new Uint8Array(res.data.buffer, res.data.byteOffset, res.data.byteLength);
  }

  async clearHalt(direction: 'in' | 'out', endpointNumber: number): Promise<void> {
    try {
      await this.device.clearHalt(direction, endpointNumber);
    } catch {
      /* Ignore if endpoint is not halted. */
    }
  }
}

export interface RecordedExec {
  addr: number;
}

/**
 * In-memory Ambarella A5s DFU simulator for unit tests and browser dry-run
 * simulation without physical camera hardware.
 */
export class MockUsbTransport implements UsbTransport {
  public opened = false;
  public readonly memory = new Map<number, number>();
  public readonly regWrites: Array<{ addr: number; val: number }> = [];
  public execCalls: RecordedExec[] = [];
  public writeCount = 0;
  public readCount = 0;
  public bulkWriteCount = 0;
  private readonly recordAllWrites: boolean;
  private readonly simulatedDelayEvery: number;
  private fastLoaderActive = false;
  private pendingWrite: { dst: number; len: number; received: number; buf: Uint8Array } | null =
    null;
  private pendingReply: Uint8Array | null = null;

  constructor(opts?: { recordAllWrites?: boolean; simulatedDelayEvery?: number }) {
    this.recordAllWrites = opts?.recordAllWrites ?? false;
    this.simulatedDelayEvery = opts?.simulatedDelayEvery ?? 0;
  }

  private writeBytesToMem(startAddr: number, data: Uint8Array): void {
    for (let i = 0; i < data.byteLength; i++) {
      const a = (startAddr + i) >>> 0;
      const wordAddr = (a & ~3) >>> 0;
      const shift = (a & 3) * 8;
      const cur = this.memory.get(wordAddr) ?? 0;
      const next = ((cur & ~(0xff << shift)) | (data[i] << shift)) >>> 0;
      this.memory.set(wordAddr, next);
    }
  }

  private makeReply(m: number, r1: number, r2: number, r3: number): Uint8Array {
    const out = new Uint8Array(16);
    const dv = new DataView(out.buffer);
    dv.setUint32(0, m >>> 0, true);
    dv.setUint32(4, r1 >>> 0, true);
    dv.setUint32(8, r2 >>> 0, true);
    dv.setUint32(12, r3 >>> 0, true);
    return out;
  }

  getDeviceInfo(): DeviceInfo {
    return {
      vendorId: AMBARELLA_VID,
      productId: AMBARELLA_A5S_PID,
      productName: 'Simulated Gen 1 Nest Cam Outdoor (Ambarella A5s)',
      manufacturerName: 'Ambarella (Simulation)',
      simulated: true,
    };
  }

  async open(): Promise<void> {
    this.opened = true;
  }

  async close(): Promise<void> {
    this.opened = false;
  }

  async controlIn(
    request: number,
    value: number,
    index: number,
    length: number,
  ): Promise<Uint8Array> {
    if (!this.opened) throw new Error('Mock USB device not opened');
    if (request !== 0 || length !== 4) {
      throw new Error(`Unexpected controlIn(req=${request}, len=${length})`);
    }
    const addr = (((value & 0xffff) << 16) | (index & 0xffff)) >>> 0;
    this.readCount++;
    const val = this.memory.get(addr) ?? 0;
    const out = new Uint8Array(4);
    out[0] = val & 0xff;
    out[1] = (val >>> 8) & 0xff;
    out[2] = (val >>> 16) & 0xff;
    out[3] = (val >>> 24) & 0xff;
    return out;
  }

  async controlOut(
    request: number,
    _value: number,
    _index: number,
    data: Uint8Array,
  ): Promise<void> {
    if (!this.opened) throw new Error('Mock USB device not opened');
    if (request !== 0 || data.byteLength !== 12) {
      throw new Error(`Unexpected controlOut(req=${request}, len=${data.byteLength})`);
    }
    const addr =
      (data[4] | (data[5] << 8) | (data[6] << 16) | (data[7] << 24)) >>> 0;
    const val =
      (data[8] | (data[9] << 8) | (data[10] << 16) | (data[11] << 24)) >>> 0;

    if (data[3] === 0x30 && data[2] === 0x00) {
      this.writeCount++;
      this.memory.set(addr, val);
      if (this.recordAllWrites || addr < 0xc0000000) {
        this.regWrites.push({ addr, val });
      }
      if (this.simulatedDelayEvery > 0 && this.writeCount % this.simulatedDelayEvery === 0) {
        await new Promise((r) => setTimeout(r, 0));
      }
      return;
    }

    if (data[2] === 0x01 && data[3] === 0x00) {
      this.execCalls.push({ addr });
      if (addr === FAST_LOADER_ADDR) {
        this.fastLoaderActive = true;
      }
      return;
    }

    throw new Error(`Unknown Ambarella command header: ${Array.from(data.slice(0, 4)).join(',')}`);
  }

  async bulkOut(endpointNumber: number, data: Uint8Array): Promise<number> {
    if (!this.opened) throw new Error('Mock USB device not opened');
    if (!this.fastLoaderActive || endpointNumber !== 1) {
      throw new Error(`Mock bulkOut(EP${endpointNumber}) not active`);
    }
    this.bulkWriteCount++;

    if (this.pendingWrite) {
      const pw = this.pendingWrite;
      const take = Math.min(data.byteLength, pw.len - pw.received);
      pw.buf.set(data.subarray(0, take), pw.received);
      pw.received += take;
      if (pw.received >= pw.len) {
        this.writeBytesToMem(pw.dst, pw.buf);
        const checksum = crc32(pw.buf);
        this.pendingReply = this.makeReply(FAST_LOADER_MAGIC_OK, checksum, pw.len, pw.dst);
        this.pendingWrite = null;
      }
      if (this.simulatedDelayEvery > 0) {
        await new Promise((r) => setTimeout(r, 2));
      }
      return data.byteLength;
    }

    if (data.byteLength < 16) {
      throw new Error(`Mock fast loader command too short (${data.byteLength} B)`);
    }
    const dv = new DataView(data.buffer, data.byteOffset, data.byteLength);
    const magic = dv.getUint32(0, true);
    if (magic !== FAST_LOADER_MAGIC_CMD) {
      throw new Error(`Mock fast loader bad magic 0x${magic.toString(16)}`);
    }
    const op = dv.getUint32(4, true);
    const arg1 = dv.getUint32(8, true);
    const arg2 = dv.getUint32(12, true);

    if (op === FAST_CMD_PING) {
      this.pendingReply = this.makeReply(FAST_LOADER_MAGIC_OK, 0xa55a0002, 0x0002002c, 0x00070308);
    } else if (op === FAST_CMD_WRITE_MEM) {
      if (arg2 === 0) {
        this.pendingReply = this.makeReply(FAST_LOADER_MAGIC_OK, crc32(new Uint8Array(0)), 0, arg1);
      } else {
        this.pendingWrite = { dst: arg1, len: arg2, received: 0, buf: new Uint8Array(arg2) };
      }
    } else if (op === FAST_CMD_WRITE_WORD) {
      this.memory.set(arg1, arg2);
      this.pendingReply = this.makeReply(FAST_LOADER_MAGIC_OK, arg2, arg1, 0);
    } else if (op === FAST_CMD_READ_WORD) {
      this.pendingReply = this.makeReply(FAST_LOADER_MAGIC_OK, this.memory.get(arg1) ?? 0, arg1, 0);
    } else if (op === FAST_CMD_EXEC) {
      this.execCalls.push({ addr: arg1 });
      this.pendingReply = this.makeReply(FAST_LOADER_MAGIC_OK, arg1, 0, 0);
    } else {
      throw new Error(`Mock fast loader unknown op ${op}`);
    }
    return data.byteLength;
  }

  async bulkIn(endpointNumber: number, length: number): Promise<Uint8Array> {
    if (!this.opened) throw new Error('Mock USB device not opened');
    if (!this.fastLoaderActive || endpointNumber !== 1 || !this.pendingReply) {
      throw new Error(`Mock bulkIn(EP${endpointNumber}) has no reply`);
    }
    const r = this.pendingReply.subarray(0, Math.min(length, this.pendingReply.byteLength));
    this.pendingReply = null;
    return r;
  }

  readBytes(startAddr: number, length: number): Uint8Array {
    const out = new Uint8Array(length);
    for (let i = 0; i < length; i++) {
      const a = (startAddr + i) >>> 0;
      const word = this.memory.get((a & ~3) >>> 0) ?? 0;
      out[i] = (word >>> ((a & 3) * 8)) & 0xff;
    }
    return out;
  }
}
