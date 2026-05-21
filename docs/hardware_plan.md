# Hardware Plan (Initial)

## Fast path (ESP32-S3 ADC)

- Start with one ADC1 channel.
- Keep input in safe ADC range.
- Use protection resistor + divider as needed.

## Precision path (ADS1256)

Suggested starter mapping:

- SCLK: GPIO12
- MOSI(DIN): GPIO11
- MISO(DOUT): GPIO13
- CS: GPIO14
- DRDY: GPIO21
- RESET: optional GPIO
- SYNC/PDWN: optional GPIO

Use DRDY interrupt for sample-ready timing.

## Safety

- Common ground required.
- Ensure ADS1256 digital outputs are 3.3V-safe to ESP32-S3.
- Avoid mains/high-voltage signals.
