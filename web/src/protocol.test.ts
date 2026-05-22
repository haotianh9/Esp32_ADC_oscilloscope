import { describe, expect, it } from "vitest";
import { buildTestFrame, FrameParser, SampleFormat, Source } from "./protocol";

describe("FrameParser", () => {
  it("decodes U16 ESP ADC frames", () => {
    const parser = new FrameParser();
    const result = parser.push(buildTestFrame({ samples: [0, 2048, 4095] }));

    expect(result.frames).toHaveLength(1);
    expect(result.frames[0].source).toBe(Source.EspAdc);
    expect(result.frames[0].samples).toEqual(new Float32Array([0, 2048, 4095]));
  });

  it("decodes sign-extended ADS1256 samples", () => {
    const parser = new FrameParser();
    const result = parser.push(
      buildTestFrame({
        source: Source.Ads1256,
        format: SampleFormat.S24InI32,
        samples: [-1, -8388608, 8388607],
      }),
    );

    expect(result.frames).toHaveLength(1);
    expect(Array.from(result.frames[0].samples)).toEqual([-1, -8388608, 8388607]);
  });

  it("recovers after non-frame bytes and reports JSON text lines", () => {
    const parser = new FrameParser();
    const prefix = new TextEncoder().encode('{"type":"ack","cmd":"status"}\nnoise');
    const frame = buildTestFrame({ samples: [7, 8] });
    const combined = new Uint8Array(prefix.byteLength + frame.byteLength);
    combined.set(prefix);
    combined.set(frame, prefix.byteLength);

    const result = parser.push(combined);

    expect(result.textLines).toEqual(['{"type":"ack","cmd":"status"}']);
    expect(result.frames).toHaveLength(1);
    expect(result.frames[0].samples[0]).toBe(7);
    expect(result.droppedBytes).toBe(0);
  });

  it("counts binary garbage before a valid frame as dropped bytes", () => {
    const parser = new FrameParser();
    const prefix = new Uint8Array([0x00, 0x80, 0x00, 0x80]);
    const frame = buildTestFrame({ samples: [7, 8] });
    const combined = new Uint8Array(prefix.byteLength + frame.byteLength);
    combined.set(prefix);
    combined.set(frame, prefix.byteLength);

    const result = parser.push(combined);

    expect(result.frames).toHaveLength(1);
    expect(result.droppedBytes).toBe(prefix.byteLength);
  });

  it("holds partial frames until enough bytes arrive", () => {
    const parser = new FrameParser();
    const frame = buildTestFrame({ samples: [1, 2, 3, 4] });

    const first = parser.push(frame.slice(0, 12));
    const second = parser.push(frame.slice(12));

    expect(first.frames).toHaveLength(0);
    expect(second.frames).toHaveLength(1);
  });

  it("rejects bad CRC and recovers on the next frame", () => {
    const parser = new FrameParser();
    const bad = buildTestFrame({ samples: [1, 2, 3] });
    bad[bad.length - 1] ^= 0xff;
    const good = buildTestFrame({ samples: [9, 10] });
    const combined = new Uint8Array(bad.byteLength + good.byteLength);
    combined.set(bad);
    combined.set(good, bad.byteLength);

    const result = parser.push(combined);

    expect(result.badCrc).toBe(1);
    expect(result.frames).toHaveLength(1);
    expect(Array.from(result.frames[0].samples)).toEqual([9, 10]);
  });
});
