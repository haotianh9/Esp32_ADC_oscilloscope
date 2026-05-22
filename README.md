# ESP32 ADC Oscilloscope

Dual-mode poor-man oscilloscope for an ESP32-S3 DevKit-style board:

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

The firmware and web app are standalone and do not build against copied reference projects. A local `references/` folder may be used for temporary study checkouts, but it is ignored by git.

## Reference Projects Consulted

This project was informed by several open-source projects and official examples. They are references only, not dependencies, and no checked-out reference repo is required to build or run this codebase.

- Espressif ESP-IDF examples: [TinyUSB serial device](https://github.com/espressif/esp-idf/tree/master/examples/peripherals/usb/device/tusb_serial_device) and [ADC continuous read](https://github.com/espressif/esp-idf/tree/master/examples/peripherals/adc/continuous_read)
- [MatAtBread/esp-scope](https://github.com/MatAtBread/esp-scope): ESP32 browser oscilloscope behavior and UI ideas
- [AndyAiCardputer/cardputer-oscilloscope](https://github.com/AndyAiCardputer/cardputer-oscilloscope): ESP32-S3 trigger and measurement ideas
- [YordanYanakiev/ADS1256](https://github.com/YordanYanakiev/ADS1256): ADS1256 command/register/SPI behavior reference
- [OscarSaharoy/femtoscope](https://github.com/OscarSaharoy/femtoscope): browser Web Serial oscilloscope flow ideas
- [uPlot](https://github.com/leeoniya/uPlot): browser plotting library, installed as an npm dependency of the web app

## Current implementation

- ESP-IDF app scaffold under `firmware/`
- TinyUSB CDC JSON command/control path
- Binary data frame builder with CRC32
- GPIO18 PWM self-test output
- GPIO4 / ADC1_CH3 continuous ADC streaming
- ADS1256 SPI bring-up and single-channel DRDY-driven streaming
- Browser app under `web/` using Web Serial, a Worker parser, and uPlot
- Data/control over both the USB-UART bridge and native TinyUSB CDC
- Protocol parser tests for frame sync, CRC, partial frames, and sample decode
- Native ADC max-rate burst benchmark command

## Serial Port

Use the serial port assigned by your OS for flashing and first bring-up. On Windows this will usually look like `COMx`; on Linux/macOS it will usually look like `/dev/ttyUSBx`, `/dev/ttyACMx`, or `/dev/cu.*`.

## First bring-up

1. Install ESP-IDF and Node.js if they are not on PATH.
2. Build firmware:

```powershell
cd firmware
idf.py set-target esp32s3
idf.py build
idf.py -p <PORT> flash
```

3. Install and run the browser app:

```powershell
cd web
npm install
npm run dev
```

4. Open the Vite localhost URL in Chrome or Edge.
5. Click **Connect**, choose the CH343/ESP32 serial device, then click **Stream**.
6. Start with `1 kSPS`, `115200` baud, and `50 Hz` PWM. Click **Status** and **Probe ADC** before streaming. Once the square wave is stable, raise the sample rate and baud.

## Native ADC benchmark result

With only the GPIO18 divider node connected to GPIO4, the native ADC path was verified at the ESP-IDF continuous driver limit:

```text
ADC sample rate: 83,333 samples/s
Sample interval: about 12.0 us/sample
Practical square-wave viewing: about 8-10 kHz
Edge/frequency detection: up to about 30-40 kHz, heavily under-sampled
```

The UART bring-up path cannot stream every sample at this rate, so use the firmware-side `adc_burst_test` command for max-rate timing tests.

See `docs/native_adc_benchmark.md`.

## ADS1256 working range

The current ADS1256 module behaves as a `2.5 V` reference ADC even though the module is powered from `5 V`. With `PGA=1`, use it first for the verified `AIN0-AINCOM` self-test range of roughly `0.07 V` to `2.14 V`.

For detailed PGA voltage ranges and sample-rate guidance, see `docs/ads1256_range.md`.

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
