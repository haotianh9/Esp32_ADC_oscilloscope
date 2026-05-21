# ESP32 ADC Oscilloscope (Dual-Mode Plan)

This repository is structured for a **dual-mode poor-man oscilloscope**:

- **Mode A (Fast scope):** ESP32-S3 native ADC (fast, lower precision)
- **Mode B (Precision mode):** ADS1256 (slower, much higher resolution)

## Repository structure

```text
firmware/      ESP-IDF firmware (acquisition, trigger, USB CDC)
web/           Browser client (Web Serial + plotting)
docs/          Hardware and implementation notes
protocol/      Binary frame format + command templates
templates/     Starter config/templates for bring-up
```

## Quick implementation phases

1. **Phase 1:** ESP32 native ADC + USB streaming + browser plot
2. **Phase 2:** Firmware-side trigger + pre-trigger ring buffer
3. **Phase 3:** ADS1256 single-channel DRDY-driven read path
4. **Phase 4:** ADS1256 trigger/calibration
5. **Phase 5:** Better analog front-end (protection, attenuation, coupling)

## Hardware notes

- Do not use GPIO19/GPIO20 for ADC input on DevKitC-1 (USB D-/D+).
- ADS1256 digital side must be safe for ESP32-S3 3.3V GPIO.
- Start ADS1256 SPI clock conservatively (e.g. 1 MHz).
- Do not connect unknown/high-voltage signals directly.

## Data/control protocol approach

- **Data path:** binary frames (sample blocks + metadata + CRC)
- **Control path:** JSON line commands (low bandwidth)

See:
- `protocol/frame_v1.md`
- `protocol/control_commands.example.jsonl`

## Next step

Use `firmware/README.md` and `web/README.md` to scaffold each side with ESP-IDF and Vite/TypeScript.
