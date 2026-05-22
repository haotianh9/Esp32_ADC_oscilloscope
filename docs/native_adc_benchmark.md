# Native ADC Benchmark

Test date: 2026-05-22

Hardware state:

```text
GPIO18 PWM -- 10k -- ADC node -- 20k -- GND
                       |
                       +-- 1k -- GPIO4 / ADC1_CH3
```

ADS1256 was disconnected from the node for this benchmark.

## Static validation

```jsonl
{"cmd":"set_pwm","gpio":18,"freq_hz":1000,"duty_percent":0,"enabled":true}
{"cmd":"adc_probe"}
{"cmd":"set_pwm","gpio":18,"freq_hz":1000,"duty_percent":100,"enabled":true}
{"cmd":"adc_probe"}
```

Observed:

```text
PWM low:  raw ADC = 0
PWM high: raw ADC ~= 2640
```

The high level is roughly:

```text
2640 / 4095 * 3.3 V ~= 2.13 V
```

This is close to the expected divider level of about `2.2 V`.

## Max-rate method

UART streaming at `115200` baud cannot carry all samples from the ADC at `83,333 SPS`, so max-rate timing is measured with a firmware-side burst command:

```jsonl
{"cmd":"adc_burst_test","fs":83333,"samples":8192,"pwm_hz":10000,"duty_percent":50,"timeout_ms":1200}
```

The command captures internally, measures edges, and returns compact JSON. This avoids serial bandwidth limits.

## Result

ADC sample rate:

```text
83,333 samples/s
12.0 us/sample
Nyquist frequency ~= 41.7 kHz
```

Measured with GPIO18 hardware PWM self-test:

| PWM requested | Measured | Samples per cycle |
| ------------: | -------: | ----------------: |
| 50 Hz         | 49.9 Hz  | ~1667             |
| 1 kHz         | 1000 Hz  | ~83               |
| 2 kHz         | 2000 Hz  | ~42               |
| 5 kHz         | 5002 Hz  | ~17               |
| 8 kHz         | 8006 Hz  | ~10               |
| 10 kHz        | 10010 Hz | ~8                |
| 15 kHz        | 15015 Hz | ~5.5              |
| 20 kHz        | 20000 Hz | ~4.2              |
| 30 kHz        | 30030 Hz | ~2.8              |
| 40 kHz        | 40161 Hz | ~2.1              |

## Practical interpretation

- `1-5 kHz`: good waveform shape for a poor-man scope.
- `8-10 kHz`: still usable, but edges and duty shape are visibly coarse.
- `15-20 kHz`: frequency/edge timing is visible, waveform shape is sparse.
- `30-40 kHz`: near Nyquist; useful mainly to prove activity/frequency, not shape.

For live browser plotting through the CH343 UART, use lower sample rates. For full-rate capture, use firmware-side burst stats or move sustained sample streaming to native USB CDC.
