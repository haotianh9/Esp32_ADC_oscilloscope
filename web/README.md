# Web App

Browser client for the ESP32-S3 dual-mode scope.

## Stack

- Vite + TypeScript
- Web Serial API
- Worker-based binary frame parser
- uPlot waveform display
- Vitest parser tests

## Run

```text
npm install
npm run dev
```

Open the local Vite URL in Chrome or Edge, then connect to the ESP32 serial device.

## Build and test

```text
npm run build
npm test
```

## Implemented controls

- Connect/disconnect/status
- Source select: ESP32 ADC or ADS1256
- Sample rate
- GPIO18 PWM self-test control
- ESP ADC attenuation
- ADS1256 PGA/buffer/register smoke test
- Trigger arm settings
- Raw/volts display, clear, CSV export
