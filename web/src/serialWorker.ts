import { FrameParser, type ScopeFrame } from "./protocol";

type WorkerCommand = { type: "chunk"; chunk: Uint8Array } | { type: "reset" };

let parser = new FrameParser();

function post(type: string, payload: Record<string, unknown> = {}): void {
  self.postMessage({ type, ...payload });
}

function postFrames(frames: ScopeFrame[]): void {
  post("frames", {
    frames: frames.map((frame) => ({
      version: frame.version,
      type: frame.type,
      seq: frame.seq,
      source: frame.source,
      channelmask: frame.channelmask,
      sampleHz: frame.sampleHz,
      t0Us: frame.t0Us.toString(),
      dtNs: frame.dtNs,
      format: frame.format,
      nsamples: frame.nsamples,
      samples: frame.samples,
    })),
  });
}

self.onmessage = (event: MessageEvent<WorkerCommand>) => {
  const message = event.data;
  if (message.type === "reset") {
    parser = new FrameParser();
    post("stats", { droppedBytes: 0, badCrc: 0 });
    return;
  }

  const result = parser.push(message.chunk);
  if (result.textLines.length) {
    post("text", { lines: result.textLines });
  }
  if (result.frames.length) {
    postFrames(result.frames);
  }
  if (result.droppedBytes || result.badCrc) {
    post("stats", {
      droppedBytes: parser.droppedBytes,
      badCrc: parser.badCrc,
      droppedPreview: result.droppedPreview,
    });
  }
};
