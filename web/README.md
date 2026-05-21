# Web App Scaffold

Suggested stack:

- Vite + TypeScript
- Web Serial API (Chromium)
- Worker-based parser
- uPlot for waveform rendering

## Suggested layout

```text
web/
  package.json
  tsconfig.json
  index.html
  src/
    main.ts
    serialWorker.ts
    protocol.ts
    plot.ts
```

## Design notes

- Parse serial frames in a Worker.
- Keep raw samples in typed arrays.
- Decimate for display and refresh chart at 30-60 Hz.
- Use firmware-side trigger for reliable captures.
