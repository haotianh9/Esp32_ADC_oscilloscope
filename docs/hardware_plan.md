# Hardware Plan (Initial)

## Self-test signal

Native ADC bring-up wiring:

```text
GPIO18 PWM -- 10k -- ADC node -- 20k -- GND
                       |
                       +-- 1k -- GPIO4 ADC
```

With a 3.3 V PWM signal, the ADC node high level is about:

```text
3.3 V * 20k / (10k + 20k) = 2.2 V
```

Bring-up check:

```jsonl
{"cmd":"set_pwm","freq_hz":50,"duty_percent":100,"enabled":true}
{"cmd":"status"}
{"cmd":"adc_probe"}
```

At 100% duty, `status` should show `gpio.pwm:1`, `gpio.adc:1`, and `adc_probe` should report raw ADC counts well above zero. If `gpio.pwm` is 1 but `gpio.adc` and the ADC probe stay near zero, the GPIO18 signal is not reaching the GPIO4 ADC node.

Verified native ADC result with only GPIO4 connected:

```text
PWM low:  raw ADC = 0
PWM high: raw ADC ~= 2640
```

## ADS1256 self-test wiring

For the external ADC phase, keep the same GPIO18 divider but replace the GPIO4 measurement connection with ADS1256 AIN0:

```text
GPIO18 PWM -- 10k -- ADC node -- 20k -- GND
                       |
                       +-- 1k -- ADS1256 AIN0

ADS1256 AINCOM -------------- GND
ESP32 GND ------------------- ADS1256 GND
```

Use single-ended `AIN0-AINCOM` first. Start at `1000 SPS`, PGA `1`, buffer off:

```jsonl
{"cmd":"set_source","source":"ads1256"}
{"cmd":"set_ads1256","fs":1000,"channel":"AIN0-AINCOM","pga":1,"vref_mv":2500,"buffer":false}
{"cmd":"ads1256_regs"}
{"cmd":"ads1256_scan"}
{"cmd":"pwm_ads_probe","channel":"AIN0-AINCOM","settle_ms":100,"discard":3,"samples":8}
{"cmd":"stream","enabled":true}
```

Expected first behavior: `ads1256_regs` should return valid register values, then `ads1256_scan` / `pwm_ads_probe` should show AIN0 changing between the PWM low and high levels. In volts mode, the expected high level is near the divider output, about `2.2 V` before ADC/front-end loading and calibration error.

Use `vref_mv=2500` for the current ADS1256 module unless a DMM measurement of the module reference pins says otherwise. The module can be powered from 5 V while its ADC reference remains 2.5 V.

See `docs/ads1256_range.md` for the external ADC voltage ranges, PGA table, and practical sample-rate limits.

## Shared-node verification

The divider node was also tested with both measurement paths connected at the same time:

```text
GPIO18 PWM -- 10k -- ADC node -- 20k -- GND
                       |
                       +-- GPIO4 / ADC1_CH3
                       |
                       +-- ADS1256 AIN0

ADS1256 AINCOM -------------- GND
ESP32 GND ------------------- ADS1256 GND
```

Observed result:

```text
ESP32 ADC:
  PWM low:  raw avg ~= 61
  PWM high: raw avg ~= 2570

ADS1256, AIN0-AINCOM, PGA=1, VREF=2.5 V:
  PWM low:  ~= 73 mV
  PWM high: ~= 2.144 V
```

Conclusion: the shared node works for the current self-test signal. The high level is slightly lower than the GPIO4-only case, which is acceptable for this bring-up and should be calibrated later if precise voltage readings matter.

## Fast path (ESP32-S3 ADC)

- Start with GPIO4 / ADC1_CH3.
- Keep input in safe ADC range.
- Use protection resistor + divider as needed.

## Precision path (ADS1256)

Suggested starter mapping:

- SCLK: GPIO12
- MOSI(DIN): GPIO11
- MISO(DOUT): GPIO13
- CS: GPIO14
- DRDY: GPIO21
- RESET: GPIO15
- SYNC/PDWN: GPIO16

Use DRDY interrupt for sample-ready timing.

## Safety

- Common ground required.
- Ensure ADS1256 digital outputs are 3.3V-safe to ESP32-S3.
- Avoid mains/high-voltage signals.
