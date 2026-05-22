import uPlot from "uplot";
import "uplot/dist/uPlot.min.css";
import type { ScopeFrame } from "./protocol";
import { SampleFormat, Source } from "./protocol";

export type UnitMode = "raw" | "volts";

export class WaveformPlot {
  private plot: uPlot;
  private x: number[] = [];
  private y: number[] = [];
  private sampleCursor = 0;
  private windowSeconds = 1;
  private maxPoints = 8000;

  constructor(target: HTMLElement) {
    this.plot = new uPlot(
      {
        width: target.clientWidth || 900,
        height: 420,
        cursor: { drag: { x: true, y: true } },
        scales: {
          x: { time: false },
          y: { auto: true },
        },
        axes: [
          { label: "Time (s)", stroke: "#596275", grid: { stroke: "#d9dee8" } },
          { label: "Signal", stroke: "#596275", grid: { stroke: "#edf0f5" } },
        ],
        series: [
          {},
          {
            label: "CH1",
            stroke: "#0f766e",
            width: 2,
          },
        ],
      },
      [this.x, this.y],
      target,
    );

    const resizeObserver = new ResizeObserver(() => {
      this.plot.setSize({ width: target.clientWidth || 900, height: 420 });
    });
    resizeObserver.observe(target);
  }

  clear(): void {
    this.x = [];
    this.y = [];
    this.sampleCursor = 0;
    this.plot.setData([this.x, this.y]);
  }

  setWindow(seconds: number): void {
    this.windowSeconds = Math.max(0.05, seconds);
    this.trim();
    this.plot.setData([this.x, this.y]);
  }

  appendFrame(frame: ScopeFrame, unitMode: UnitMode, adsPga: number, adsVrefVolts: number): void {
    const sampleHz = Math.max(1, frame.sampleHz);
    const stride = Math.max(1, Math.floor(frame.nsamples / 1200));

    for (let i = 0; i < frame.samples.length; i += stride) {
      this.x.push(this.sampleCursor / sampleHz);
      this.y.push(this.convert(frame, frame.samples[i], unitMode, adsPga, adsVrefVolts));
      this.sampleCursor += stride;
    }

    this.trim();
    this.plot.setData([this.x, this.y]);
  }

  toCsv(): string {
    const rows = ["time_s,value"];
    for (let i = 0; i < this.x.length; i += 1) {
      rows.push(`${this.x[i]},${this.y[i]}`);
    }
    return rows.join("\n");
  }

  private trim(): void {
    const minTime = this.x.length ? this.x[this.x.length - 1] - this.windowSeconds : 0;
    let keepFrom = 0;
    while (keepFrom < this.x.length && this.x[keepFrom] < minTime) {
      keepFrom += 1;
    }
    if (keepFrom > 0) {
      this.x = this.x.slice(keepFrom);
      this.y = this.y.slice(keepFrom);
    }
    if (this.x.length > this.maxPoints) {
      this.x = this.x.slice(this.x.length - this.maxPoints);
      this.y = this.y.slice(this.y.length - this.maxPoints);
    }
  }

  private convert(
    frame: ScopeFrame,
    sample: number,
    unitMode: UnitMode,
    adsPga: number,
    adsVrefVolts: number,
  ): number {
    if (unitMode === "raw") {
      return sample;
    }
    if (frame.source === Source.EspAdc && frame.format === SampleFormat.U16) {
      return (sample * 3.3) / 4095;
    }
    if (frame.source === Source.Ads1256 && frame.format === SampleFormat.S24InI32) {
      return (sample / 8388607) * ((2 * adsVrefVolts) / Math.max(1, adsPga));
    }
    return sample;
  }
}
