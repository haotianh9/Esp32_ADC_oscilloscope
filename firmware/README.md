# Firmware (ESP-IDF)

ESP-IDF firmware for the ESP32-S3 dual-mode scope.

## Build

```powershell
idf.py set-target esp32s3
idf.py build
idf.py -p COM9 flash
```

`COM9` is the active CH343 USB-UART port found on this machine. Re-run the port search if Windows changes it.

## Implemented layout

```text
firmware/
  CMakeLists.txt
  sdkconfig.defaults
  main/
    CMakeLists.txt
    app_main.c
    adc_fast.c/.h
    ads1256.c/.h
    command.c/.h
    frame.c/.h
    scope_config.h
    selftest_pwm.c/.h
    trigger.c/.h
    usb_stream.c/.h
```

## Hardware defaults

```text
GPIO18            PWM self-test output
GPIO4 / ADC1_CH3  native ADC input
GPIO12            ADS1256 SCLK
GPIO11            ADS1256 DIN / MOSI
GPIO13            ADS1256 DOUT / MISO
GPIO14            ADS1256 CS
GPIO21            ADS1256 DRDY
GPIO15            ADS1256 RESET
GPIO16            ADS1256 SYNC/PDWN
```

## Command examples

```jsonl
{"cmd":"set_pwm","freq_hz":50,"duty_percent":50,"enabled":true}
{"cmd":"set_source","source":"esp_adc"}
{"cmd":"set_adc","fs":1000,"channels":[4],"atten":"12db"}
{"cmd":"adc_probe"}
{"cmd":"adc_burst_test","fs":83333,"samples":8192,"pwm_hz":10000,"duty_percent":50,"timeout_ms":1200}
{"cmd":"set_source","source":"ads1256"}
{"cmd":"set_ads1256","fs":1000,"channel":"AIN0-AINCOM","pga":1,"vref_mv":2500,"buffer":false}
{"cmd":"ads1256_regs"}
{"cmd":"ads1256_scan"}
{"cmd":"pwm_ads_probe","channel":"AIN0-AINCOM","settle_ms":100,"discard":3,"samples":8}
{"cmd":"stream","enabled":true}
{"cmd":"status"}
{"cmd":"stop"}
```

## Notes

- The firmware uses native USB CDC for the app protocol.
- The firmware also mirrors the protocol over UART0 at 115200 baud for first bring-up on the CH343 `COM9` bridge.
- `adc_probe` reports GPIO4 oneshot ADC min/max/avg and is useful before starting the continuous stream.
- `adc_burst_test` captures internally and returns timing stats as JSON, avoiding UART bandwidth limits at high ADC sample rates.
- ESP ADC samples are sent as unsigned 16-bit raw counts.
- ADS1256 samples are sent as sign-extended 32-bit integers.
- ADS1256 voltage conversion uses the configured reference voltage. The current module behaved like a `2.5 V` reference even though the board is powered from `5 V`, so examples use `vref_mv=2500`.
- The trigger engine currently reports trigger events; full pre/post capture buffering is the next firmware expansion.

## Native ADC timing result

The GPIO18 divider self-test signal was verified with only GPIO4 connected:

- Static low: raw `0`
- Static high: raw about `2640`
- Continuous stream at `10 kSPS`, 50 Hz PWM: clean square wave
- Internal burst at `83,333 SPS`: about `12.0 us/sample`

Useful native ADC expectations:

- 1 kHz: about 83 samples/cycle
- 5 kHz: about 17 samples/cycle
- 10 kHz: about 8 samples/cycle
- 20 kHz: about 4 samples/cycle
- 40 kHz: about 2 samples/cycle, only edge/frequency detection

For live plotting over the CH343 UART, use lower sample rates. For max-rate captures, use `adc_burst_test` or move the streaming path to native USB CDC.

## ADS1256 range summary

The current external ADC path is verified with `AIN0-AINCOM`, `PGA=1`, `buffer=false`, and `vref_mv=2500`.

Observed self-test range:

```text
PWM low:  about 0.07 V
PWM high: about 2.14 V
```

Use ADS1256 mode for DC, sensors, calibrated slow logging, and low-frequency precision waveforms. Use ESP32 ADC mode for faster edges and signals above a few kHz.

Detailed range notes are in `docs/ads1256_range.md`.
