import "./style.css";
import { WaveformPlot, type UnitMode } from "./plot";
import { buildCommand, Source, SampleFormat, type ScopeFrame } from "./protocol";

type WorkerFrame = Omit<ScopeFrame, "t0Us" | "payload"> & { t0Us: string };

const app = document.querySelector<HTMLDivElement>("#app");
if (!app) {
  throw new Error("Missing #app");
}

app.innerHTML = `
  <main class="shell">
    <section class="toolbar">
      <div class="brand">
        <h1>ESP32-S3 Dual Scope</h1>
        <span id="statusText">Disconnected</span>
      </div>
      <div class="actions">
        <button id="connectBtn">Connect</button>
        <button id="disconnectBtn" disabled>Disconnect</button>
        <button id="statusBtn">Status</button>
      </div>
    </section>

    <section class="layout">
      <aside class="controls">
        <fieldset>
          <legend>Acquisition</legend>
          <label>Source
            <select id="sourceSelect">
              <option value="esp_adc">ESP32 ADC</option>
              <option value="ads1256">ADS1256</option>
            </select>
          </label>
          <label>Sample rate
              <input id="sampleRateInput" type="number" min="3" max="83333" value="1000" />
          </label>
          <label>Serial baud
            <select id="baudSelect">
              <option value="115200">115200</option>
              <option value="921600">921600</option>
            </select>
          </label>
          <label>Input format
            <select id="inputFormatSelect">
              <option value="framed">Framed protocol</option>
              <option value="raw_u8">Raw bytes debug</option>
            </select>
          </label>
          <div class="buttonRow">
            <button id="streamBtn">Stream</button>
            <button id="stopBtn">Stop</button>
          </div>
        </fieldset>

        <fieldset>
          <legend>Self-Test PWM</legend>
          <label>Frequency
            <input id="pwmFreqInput" type="number" min="1" max="4000" value="50" />
          </label>
          <label>Duty
            <input id="pwmDutyInput" type="number" min="0" max="100" value="50" />
          </label>
          <label class="inline">
            <input id="pwmEnabledInput" type="checkbox" checked />
            Enabled
          </label>
          <button id="pwmBtn">Apply PWM</button>
        </fieldset>

        <fieldset>
          <legend>ESP32 ADC</legend>
          <label>GPIO
            <input type="text" value="GPIO4 / ADC1_CH3" disabled />
          </label>
          <label>Attenuation
            <select id="attenSelect">
              <option value="12db">12 dB</option>
              <option value="6db">6 dB</option>
              <option value="2.5db">2.5 dB</option>
              <option value="0db">0 dB</option>
            </select>
          </label>
          <div class="buttonRow">
            <button id="adcBtn">Apply ADC</button>
            <button id="adcProbeBtn">Probe ADC</button>
          </div>
        </fieldset>

        <fieldset>
          <legend>ADS1256</legend>
          <label>Channel
            <select id="adsChannelSelect">
              <option value="AIN0-AINCOM">AIN0-AINCOM</option>
              <option value="AIN1-AINCOM">AIN1-AINCOM</option>
              <option value="AIN2-AINCOM">AIN2-AINCOM</option>
              <option value="AIN3-AINCOM">AIN3-AINCOM</option>
              <option value="AIN4-AINCOM">AIN4-AINCOM</option>
              <option value="AIN5-AINCOM">AIN5-AINCOM</option>
              <option value="AIN6-AINCOM">AIN6-AINCOM</option>
              <option value="AIN7-AINCOM">AIN7-AINCOM</option>
            </select>
          </label>
          <label>PGA
            <select id="pgaSelect">
              <option>1</option>
              <option>2</option>
              <option>4</option>
              <option>8</option>
              <option>16</option>
              <option>32</option>
              <option>64</option>
            </select>
          </label>
          <label>VREF
            <input id="adsVrefInput" type="number" min="0.5" max="5" step="0.001" value="2.500" />
          </label>
          <label class="inline">
            <input id="adsBufferInput" type="checkbox" />
            Buffer
          </label>
          <div class="buttonRow">
            <button id="adsBtn">Apply ADS</button>
            <button id="adsRegsBtn">Read Regs</button>
            <button id="adsScanBtn">Scan</button>
          </div>
        </fieldset>

        <fieldset>
          <legend>Trigger</legend>
          <label>Edge
            <select id="edgeSelect">
              <option value="rising">Rising</option>
              <option value="falling">Falling</option>
            </select>
          </label>
          <label>Level mV
            <input id="levelInput" type="number" value="1200" />
          </label>
          <label>Hysteresis mV
            <input id="hystInput" type="number" value="20" />
          </label>
          <label>Pre-trigger
            <input id="preInput" type="number" min="0" max="0.95" step="0.05" value="0.25" />
          </label>
          <label>Samples
            <input id="captureSamplesInput" type="number" min="128" value="4096" />
          </label>
          <button id="armBtn">Arm</button>
        </fieldset>
      </aside>

      <section class="scopePane">
        <div class="displayTools">
          <label>Units
            <select id="unitSelect">
              <option value="raw">Raw</option>
              <option value="volts">Volts</option>
            </select>
          </label>
          <label>Window
            <input id="windowInput" type="number" min="0.05" step="0.05" value="1" />
          </label>
          <button id="clearBtn">Clear</button>
          <button id="exportCsvBtn">Export CSV</button>
        </div>
        <div id="chart" class="chart"></div>
        <div class="metrics">
          <span id="frameCount">Frames 0</span>
          <span id="sampleCount">Samples 0</span>
          <span id="rxCount">RX 0 B</span>
          <span id="dropCount">Dropped 0</span>
          <span id="crcCount">CRC 0</span>
        </div>
        <pre id="log"></pre>
      </section>
    </section>
  </main>
`;

const worker = new Worker(new URL("./serialWorker.ts", import.meta.url), { type: "module" });
const plot = new WaveformPlot(mustGet("chart"));
let connected = false;
let framesSeen = 0;
let samplesSeen = 0;
let rxBytesSeen = 0;
let port: SerialPort | null = null;
let reader: ReadableStreamDefaultReader<Uint8Array> | null = null;
let writer: WritableStreamDefaultWriter<Uint8Array> | null = null;
let readLoopActive = false;
let wrongBaudWarned = false;

const statusText = mustGet("statusText");
const connectBtn = mustGetButton("connectBtn");
const disconnectBtn = mustGetButton("disconnectBtn");
const sourceSelect = mustGetSelect("sourceSelect");
const sampleRateInput = mustGetInput("sampleRateInput");
const pgaSelect = mustGetSelect("pgaSelect");
const unitSelect = mustGetSelect("unitSelect");

mustGetSelect("baudSelect").value = "115200";
sampleRateInput.value = "1000";

connectBtn.addEventListener("click", connect);
disconnectBtn.addEventListener("click", () => {
  void disconnect();
});
mustGetButton("statusBtn").addEventListener("click", () => send({ cmd: "status" }));
mustGetButton("streamBtn").addEventListener("click", () => {
  applySourceConfig();
  send({ cmd: "stream", enabled: true });
});
mustGetButton("stopBtn").addEventListener("click", () => send({ cmd: "stop" }));
mustGetButton("pwmBtn").addEventListener("click", () => {
  send({
    cmd: "set_pwm",
    freq_hz: numberValue("pwmFreqInput"),
    duty_percent: numberValue("pwmDutyInput"),
    enabled: mustGetInput("pwmEnabledInput").checked,
  });
});
mustGetButton("adcBtn").addEventListener("click", () => applyAdcConfig());
mustGetButton("adcProbeBtn").addEventListener("click", () => send({ cmd: "adc_probe" }));
mustGetButton("adsBtn").addEventListener("click", () => applyAdsConfig());
mustGetButton("adsRegsBtn").addEventListener("click", () => send({ cmd: "ads1256_regs" }));
mustGetButton("adsScanBtn").addEventListener("click", () => send({ cmd: "ads1256_scan" }));
mustGetButton("armBtn").addEventListener("click", () => {
  send({
    cmd: "arm",
    edge: mustGetSelect("edgeSelect").value,
    level_mv: numberValue("levelInput"),
    hyst_mv: numberValue("hystInput"),
    pre: numberValue("preInput"),
    samples: numberValue("captureSamplesInput"),
  });
});
mustGetButton("clearBtn").addEventListener("click", () => {
  plot.clear();
  framesSeen = 0;
  samplesSeen = 0;
  rxBytesSeen = 0;
  wrongBaudWarned = false;
  mustGet("rxCount").textContent = "RX 0 B";
  worker.postMessage({ type: "reset" });
  updateMetrics();
});
mustGetButton("exportCsvBtn").addEventListener("click", exportCsv);
mustGetInput("windowInput").addEventListener("change", () => {
  plot.setWindow(numberValue("windowInput"));
});

worker.onmessage = (event: MessageEvent) => {
  const message = event.data;
  if (message.type === "frames") {
    handleFrames(message.frames as WorkerFrame[]);
  } else if (message.type === "text") {
    for (const line of message.lines as string[]) {
      logLine(line);
    }
  } else if (message.type === "stats") {
    mustGet("dropCount").textContent = `Dropped ${message.droppedBytes}`;
    mustGet("crcCount").textContent = `CRC ${message.badCrc}`;
    if (message.droppedPreview) {
      logLine(`Dropped preview: ${message.droppedPreview}`);
      if (!wrongBaudWarned && framesSeen === 0 && looksLikeWrongBaudPreview(message.droppedPreview)) {
        wrongBaudWarned = true;
        logLine("Wrong-baud pattern detected. Disconnect, select 115200, then reconnect to COM9.");
      }
    }
  } else if (message.type === "error") {
    logLine(`Error: ${message.message}`);
  }
};

function handleFrames(frames: WorkerFrame[]): void {
  for (const frame of frames) {
    const scopeFrame: ScopeFrame = {
      ...frame,
      t0Us: BigInt(frame.t0Us),
      payload: new Uint8Array(),
      source: frame.source as Source,
      format: frame.format as SampleFormat,
    };
    plot.appendFrame(
      scopeFrame,
      unitSelect.value as UnitMode,
      Number(pgaSelect.value),
      numberValue("adsVrefInput"),
    );
    framesSeen += 1;
    samplesSeen += frame.nsamples;
  }
  updateMetrics();
}

async function connect(): Promise<void> {
  if (!navigator.serial) {
    logLine("Web Serial is unavailable. Use Chrome or Edge on localhost/HTTPS.");
    return;
  }
  try {
    port = await navigator.serial.requestPort();
    const baudRate = Number(mustGetSelect("baudSelect").value);
    await port.open({ baudRate, bufferSize: 8192 });
    worker.postMessage({ type: "reset" });
    wrongBaudWarned = false;
    writer = port.writable?.getWriter() ?? null;
    connected = true;
    readLoopActive = true;
    connectBtn.disabled = true;
    disconnectBtn.disabled = false;
    const info = port.getInfo();
    statusText.textContent = `Connected VID ${hex(info.usbVendorId)} PID ${hex(info.usbProductId)}`;
    logLine(`Connected at ${baudRate} baud`);
    if (baudRate !== 115200) {
      logLine("Bring-up firmware is currently configured for 115200 baud.");
    }
    send({ cmd: "status" });
    void readLoop();
  } catch (error) {
    logLine(error instanceof Error ? error.message : String(error));
    await disconnect();
  }
}

async function disconnect(): Promise<void> {
  readLoopActive = false;
  try {
    await reader?.cancel();
  } catch {
    // Reader may already be closed by unplug or browser shutdown.
  }
  reader?.releaseLock();
  reader = null;
  writer?.releaseLock();
  writer = null;
  if (port) {
    try {
      await port.close();
    } catch {
      // Ignore close races on unplug.
    }
  }
  port = null;
  connected = false;
  connectBtn.disabled = false;
  disconnectBtn.disabled = true;
  statusText.textContent = "Disconnected";
}

async function readLoop(): Promise<void> {
  if (!port?.readable) {
    logLine("Connected port is not readable");
    return;
  }

  reader = port.readable.getReader();
  try {
    while (readLoopActive) {
      const { value, done } = await reader.read();
      if (done) {
        break;
      }
      if (!value) {
        continue;
      }
      const chunk = value.slice();
      rxBytesSeen += chunk.byteLength;
      mustGet("rxCount").textContent = `RX ${rxBytesSeen} B`;
      if (mustGetSelect("inputFormatSelect").value === "raw_u8") {
        appendRawBytes(chunk);
      } else {
        worker.postMessage({ type: "chunk", chunk }, [chunk.buffer]);
      }
    }
  } catch (error) {
    if (readLoopActive) {
      logLine(error instanceof Error ? error.message : String(error));
    }
  } finally {
    reader?.releaseLock();
    reader = null;
    if (readLoopActive) {
      await disconnect();
    }
  }
}

function appendRawBytes(chunk: Uint8Array): void {
  const samples = new Float32Array(chunk.length);
  for (let i = 0; i < chunk.length; i += 1) {
    samples[i] = chunk[i];
  }

  const sampleHz = Math.max(1, numberValue("sampleRateInput"));
  plot.appendFrame(
    {
      version: 0,
      type: 1,
      seq: framesSeen,
      source: Source.EspAdc,
      channelmask: 1,
      sampleHz,
      t0Us: 0n,
      dtNs: Math.floor(1_000_000_000 / sampleHz),
      format: SampleFormat.F32,
      nsamples: samples.length,
      payload: new Uint8Array(),
      samples,
    },
    "raw",
    Number(pgaSelect.value),
    numberValue("adsVrefInput"),
  );
  framesSeen += 1;
  samplesSeen += samples.length;
  updateMetrics();
}

function applySourceConfig(): void {
  send({ cmd: "set_source", source: sourceSelect.value });
  if (sourceSelect.value === "esp_adc") {
    applyAdcConfig();
  } else {
    applyAdsConfig();
  }
}

function applyAdcConfig(): void {
  send({
    cmd: "set_adc",
    fs: numberValue("sampleRateInput"),
    channels: [4],
    atten: mustGetSelect("attenSelect").value,
  });
}

function applyAdsConfig(): void {
  send({
    cmd: "set_ads1256",
    fs: numberValue("sampleRateInput"),
    channel: mustGetSelect("adsChannelSelect").value,
    pga: Number(pgaSelect.value),
    vref_mv: Math.round(numberValue("adsVrefInput") * 1000),
    buffer: mustGetInput("adsBufferInput").checked,
  });
}

function send(command: Record<string, unknown>): void {
  if (!connected || !writer) {
    logLine("Not connected");
    return;
  }
  void writer.write(buildCommand(command)).catch((error) => {
    logLine(error instanceof Error ? error.message : String(error));
  });
}

function updateMetrics(): void {
  mustGet("frameCount").textContent = `Frames ${framesSeen}`;
  mustGet("sampleCount").textContent = `Samples ${samplesSeen}`;
}

function exportCsv(): void {
  const blob = new Blob([plot.toCsv()], { type: "text/csv" });
  const url = URL.createObjectURL(blob);
  const link = document.createElement("a");
  link.href = url;
  link.download = `esp32-scope-${new Date().toISOString().replace(/[:.]/g, "-")}.csv`;
  link.click();
  URL.revokeObjectURL(url);
}

function logLine(line: string): void {
  const log = mustGet("log");
  log.textContent = `${line}\n${log.textContent ?? ""}`.slice(0, 5000);
}

function numberValue(id: string): number {
  return Number(mustGetInput(id).value);
}

function hex(value: unknown): string {
  return typeof value === "number" ? `0x${value.toString(16).padStart(4, "0")}` : "unknown";
}

function looksLikeWrongBaudPreview(preview: string): boolean {
  const hexPart = preview.split("|")[0] ?? "";
  const bytes = hexPart
    .trim()
    .split(/\s+/)
    .filter(Boolean);
  if (bytes.length < 16) {
    return false;
  }

  const zeroOr80 = bytes.filter((byte) => byte === "00" || byte === "80").length;
  return zeroOr80 / bytes.length > 0.8;
}

function mustGet(id: string): HTMLElement {
  const element = document.getElementById(id);
  if (!element) {
    throw new Error(`Missing #${id}`);
  }
  return element;
}

function mustGetInput(id: string): HTMLInputElement {
  return mustGet(id) as HTMLInputElement;
}

function mustGetSelect(id: string): HTMLSelectElement {
  return mustGet(id) as HTMLSelectElement;
}

function mustGetButton(id: string): HTMLButtonElement {
  return mustGet(id) as HTMLButtonElement;
}
