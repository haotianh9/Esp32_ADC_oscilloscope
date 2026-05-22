export const FRAME_MAGIC = 0xa55a;
export const FRAME_HEADER_LEN = 30;
export const FRAME_CRC_LEN = 4;

export enum FrameType {
  Data = 1,
  Status = 2,
  Ack = 3,
  Error = 4,
}

export enum Source {
  EspAdc = 0,
  Ads1256 = 1,
}

export enum SampleFormat {
  U16 = 1,
  S24InI32 = 2,
  F32 = 3,
}

export interface ScopeFrame {
  version: number;
  type: FrameType;
  seq: number;
  source: Source;
  channelmask: number;
  sampleHz: number;
  t0Us: bigint;
  dtNs: number;
  format: SampleFormat;
  nsamples: number;
  payload: Uint8Array;
  samples: Float32Array;
}

export interface ParserResult {
  frames: ScopeFrame[];
  droppedBytes: number;
  badCrc: number;
  textLines: string[];
  droppedPreview: string;
}

export function crc32(bytes: Uint8Array): number {
  let crc = 0xffffffff;
  for (const byte of bytes) {
    crc ^= byte;
    for (let bit = 0; bit < 8; bit += 1) {
      const mask = -(crc & 1);
      crc = (crc >>> 1) ^ (0xedb88320 & mask);
    }
  }
  return (~crc) >>> 0;
}

function readU16(view: DataView, offset: number): number {
  return view.getUint16(offset, true);
}

function readU32(view: DataView, offset: number): number {
  return view.getUint32(offset, true);
}

function decodeSamples(format: SampleFormat, payload: Uint8Array, nsamples: number): Float32Array {
  const view = new DataView(payload.buffer, payload.byteOffset, payload.byteLength);
  const samples = new Float32Array(nsamples);

  if (format === SampleFormat.U16) {
    for (let i = 0; i < nsamples && i * 2 + 1 < payload.byteLength; i += 1) {
      samples[i] = view.getUint16(i * 2, true);
    }
    return samples;
  }

  if (format === SampleFormat.S24InI32) {
    for (let i = 0; i < nsamples && i * 4 + 3 < payload.byteLength; i += 1) {
      samples[i] = view.getInt32(i * 4, true);
    }
    return samples;
  }

  if (format === SampleFormat.F32) {
    for (let i = 0; i < nsamples && i * 4 + 3 < payload.byteLength; i += 1) {
      samples[i] = view.getFloat32(i * 4, true);
    }
  }

  return samples;
}

function payloadBytes(format: SampleFormat, nsamples: number): number | null {
  if (format === SampleFormat.U16) {
    return nsamples * 2;
  }
  if (format === SampleFormat.S24InI32 || format === SampleFormat.F32) {
    return nsamples * 4;
  }
  return null;
}

export class FrameParser {
  private buffer = new Uint8Array(0);
  private textTail = "";
  private textDecoder = new TextDecoder();
  droppedBytes = 0;
  badCrc = 0;

  push(chunk: Uint8Array): ParserResult {
    const joined = new Uint8Array(this.buffer.byteLength + chunk.byteLength);
    joined.set(this.buffer, 0);
    joined.set(chunk, this.buffer.byteLength);
    this.buffer = joined;

    const frames: ScopeFrame[] = [];
    const textLines: string[] = [];
    let droppedPreview = "";
    let droppedThisPush = 0;
    let badCrcThisPush = 0;

    while (this.buffer.byteLength >= FRAME_HEADER_LEN + FRAME_CRC_LEN) {
      const magicOffset = this.findMagic(this.buffer);
      if (magicOffset < 0) {
        const keep = this.buffer[this.buffer.byteLength - 1] === 0x5a ? 1 : 0;
        const dropped = this.buffer.slice(0, this.buffer.byteLength - keep);
        const lines = this.consumeText(dropped);
        textLines.push(...lines);
        if (!looksLikeText(dropped)) {
          droppedThisPush += dropped.byteLength;
          droppedPreview ||= formatDroppedPreview(dropped);
        }
        this.buffer = this.buffer.slice(this.buffer.byteLength - keep);
        break;
      }
      if (magicOffset > 0) {
        const dropped = this.buffer.slice(0, magicOffset);
        const lines = this.consumeText(dropped);
        textLines.push(...lines);
        if (!looksLikeText(dropped)) {
          droppedPreview ||= formatDroppedPreview(dropped);
          droppedThisPush += magicOffset;
        }
        this.buffer = this.buffer.slice(magicOffset);
      }
      if (this.buffer.byteLength < FRAME_HEADER_LEN + FRAME_CRC_LEN) {
        break;
      }

      const view = new DataView(this.buffer.buffer, this.buffer.byteOffset, this.buffer.byteLength);
      const version = view.getUint8(2);
      const type = view.getUint8(3) as FrameType;
      const seq = readU32(view, 4);
      const source = view.getUint8(8) as Source;
      const channelmask = readU16(view, 9);
      const sampleHz = readU32(view, 11);
      const t0Us = view.getBigUint64(15, true);
      const dtNs = readU32(view, 23);
      const format = view.getUint8(27) as SampleFormat;
      const nsamples = readU16(view, 28);
      const payloadLen = payloadBytes(format, nsamples);

      if (payloadLen === null) {
        droppedThisPush += 2;
        this.buffer = this.buffer.slice(2);
        continue;
      }

      const frameLen = FRAME_HEADER_LEN + payloadLen + FRAME_CRC_LEN;
      if (this.buffer.byteLength < frameLen) {
        break;
      }

      const expected = readU32(view, FRAME_HEADER_LEN + payloadLen);
      const actual = crc32(this.buffer.slice(2, FRAME_HEADER_LEN + payloadLen));
      if (expected !== actual) {
        badCrcThisPush += 1;
        this.buffer = this.buffer.slice(2);
        continue;
      }

      const payload = this.buffer.slice(FRAME_HEADER_LEN, FRAME_HEADER_LEN + payloadLen);
      frames.push({
        version,
        type,
        seq,
        source,
        channelmask,
        sampleHz,
        t0Us,
        dtNs,
        format,
        nsamples,
        payload,
        samples: decodeSamples(format, payload, nsamples),
      });
      this.buffer = this.buffer.slice(frameLen);
    }

    this.droppedBytes += droppedThisPush;
    this.badCrc += badCrcThisPush;
    return { frames, droppedBytes: droppedThisPush, badCrc: badCrcThisPush, textLines, droppedPreview };
  }

  private findMagic(bytes: Uint8Array): number {
    for (let i = 0; i + 1 < bytes.byteLength; i += 1) {
      if (bytes[i] === 0x5a && bytes[i + 1] === 0xa5) {
        return i;
      }
    }
    return -1;
  }

  private consumeText(bytes: Uint8Array): string[] {
    if (bytes.byteLength === 0) {
      return [];
    }

    this.textTail += this.textDecoder.decode(bytes, { stream: true });
    const parts = this.textTail.split("\n");
    this.textTail = parts.pop() ?? "";
    return parts
      .map((line) => line.trim())
      .filter((line) => line.length > 0);
  }
}

function formatDroppedPreview(bytes: Uint8Array): string {
  const preview = bytes.slice(0, 48);
  const hex = Array.from(preview)
    .map((byte) => byte.toString(16).padStart(2, "0"))
    .join(" ");
  const text = Array.from(preview)
    .map((byte) => (byte >= 32 && byte <= 126 ? String.fromCharCode(byte) : "."))
    .join("");
  return `${hex} | ${text}`;
}

function looksLikeText(bytes: Uint8Array): boolean {
  if (bytes.byteLength === 0) {
    return true;
  }

  let printable = 0;
  for (const byte of bytes) {
    if (byte === 0x09 || byte === 0x0a || byte === 0x0d || (byte >= 0x20 && byte <= 0x7e)) {
      if (byte >= 0x20 && byte <= 0x7e) {
        printable += 1;
      }
      continue;
    }
    return false;
  }
  return printable > 0;
}

export function buildCommand(command: Record<string, unknown>): Uint8Array {
  return new TextEncoder().encode(`${JSON.stringify(command)}\n`);
}

export function buildTestFrame(params: {
  source?: Source;
  format?: SampleFormat;
  sampleHz?: number;
  samples: number[];
  seq?: number;
}): Uint8Array {
  const source = params.source ?? Source.EspAdc;
  const format = params.format ?? SampleFormat.U16;
  const sampleHz = params.sampleHz ?? 40000;
  const seq = params.seq ?? 0;
  const bytesPerSample = format === SampleFormat.U16 ? 2 : 4;
  const payload = new Uint8Array(params.samples.length * bytesPerSample);
  const payloadView = new DataView(payload.buffer);

  params.samples.forEach((sample, i) => {
    if (format === SampleFormat.U16) {
      payloadView.setUint16(i * 2, sample, true);
    } else if (format === SampleFormat.S24InI32) {
      payloadView.setInt32(i * 4, sample, true);
    } else {
      payloadView.setFloat32(i * 4, sample, true);
    }
  });

  const frame = new Uint8Array(FRAME_HEADER_LEN + payload.byteLength + FRAME_CRC_LEN);
  const view = new DataView(frame.buffer);
  view.setUint16(0, FRAME_MAGIC, true);
  view.setUint8(2, 1);
  view.setUint8(3, FrameType.Data);
  view.setUint32(4, seq, true);
  view.setUint8(8, source);
  view.setUint16(9, 1, true);
  view.setUint32(11, sampleHz, true);
  view.setBigUint64(15, 0n, true);
  view.setUint32(23, Math.floor(1_000_000_000 / sampleHz), true);
  view.setUint8(27, format);
  view.setUint16(28, params.samples.length, true);
  frame.set(payload, FRAME_HEADER_LEN);
  view.setUint32(FRAME_HEADER_LEN + payload.byteLength, crc32(frame.slice(2, FRAME_HEADER_LEN + payload.byteLength)), true);
  return frame;
}
