# ADS1256 Working Range

This note describes the practical range for the current ADS1256 precision path.

Current verified setup:

```text
ADS1256 powered from 5 V analog supply
ADS1256 reference behaves as 2.5 V
DVDD / digital logic must be 3.3 V-safe for the ESP32-S3
AIN0 connected to the GPIO18 divider node
AINCOM connected to GND
Mode: AIN0-AINCOM, PGA=1, buffer off
```

The verified self-test result is:

```text
PWM low:  about 0.07 V
PWM high: about 2.14 V
```

This matches the expected low-voltage divider signal.

## Voltage Range

The ADS1256 differential full-scale range is:

```text
Full-scale differential input = +/- (2 * VREF / PGA)
```

With the current `VREF = 2.5 V`, the code-to-voltage ranges are:

| PGA | Differential full-scale range | Best for |
| --: | ----------------------------: | -------- |
| 1   | +/- 5.000 V                   | General low-voltage signals, current self-test |
| 2   | +/- 2.500 V                   | Smaller signals up to a few volts |
| 4   | +/- 1.250 V                   | Sensor outputs around 1 V |
| 8   | +/- 0.625 V                   | Small sensor signals |
| 16  | +/- 0.3125 V                  | Low-level DC signals |
| 32  | +/- 0.15625 V                 | Very small differential signals |
| 64  | +/- 0.078125 V                | Tiny signals, only with a clean frontend |

Important: this is the differential conversion range, not permission to connect arbitrary negative or high-voltage signals. With the current single-supply wiring, each ADS1256 analog input pin must stay within the module's safe analog input limits relative to AGND and AVDD.

For this project, treat the first safe input range as:

```text
AIN0-AINCOM single-ended:
  about 0 V to about 2.2 V from the self-test divider

Do not connect:
  negative voltages
  mains
  unknown external circuits
  signals above the module/input protection range
```

If ADS1256 input buffer is enabled, the allowed analog input range is narrower. Keep buffer off for the current self-test path unless the frontend is changed and revalidated.

## Sample Rate And Signal Frequency

The ADS1256 is for precision, not fast oscilloscope work. Use it for DC, sensors, slow waveforms, and logging.

Practical first rates:

| ADS1256 data rate | Use |
| ----------------: | --- |
| 1000 SPS          | First bring-up, stable sensor/logging tests |
| 7500 SPS          | Low-frequency waveforms with more timing detail |
| 15000 SPS         | Faster slow-scope mode, less noise averaging |
| 30000 SPS         | Maximum single-channel speed, least settling margin |

Rule of thumb for waveform viewing:

```text
Good shape:     signal frequency <= sample_rate / 20
Coarse shape:   signal frequency <= sample_rate / 10
Activity only:  signal frequency near sample_rate / 4 or higher
```

Examples:

| ADS1256 rate | Good waveform shape | Coarse but useful | Not recommended for shape |
| -----------: | ------------------: | ----------------: | ------------------------: |
| 1000 SPS     | <= 50 Hz            | <= 100 Hz         | > 250 Hz                  |
| 7500 SPS     | <= 375 Hz           | <= 750 Hz         | > 1.8 kHz                 |
| 15000 SPS    | <= 750 Hz           | <= 1.5 kHz        | > 3.7 kHz                 |
| 30000 SPS    | <= 1.5 kHz          | <= 3 kHz          | > 7.5 kHz                 |

For faster PWM edges or audio-ish waveforms, use ESP32 native ADC mode instead.

## Recommended ADS1256 Bring-Up Commands

Use the current known-good configuration first:

```jsonl
{"cmd":"set_source","source":"ads1256"}
{"cmd":"set_ads1256","fs":1000,"channel":"AIN0-AINCOM","pga":1,"vref_mv":2500,"buffer":false}
{"cmd":"ads1256_regs"}
{"cmd":"pwm_ads_probe","channel":"AIN0-AINCOM","settle_ms":200,"discard":5,"samples":16}
{"cmd":"stream","enabled":true}
```

Expected `pwm_ads_probe` result with the current divider:

```text
low:  about 0.07 V
high: about 2.1 V to 2.2 V
```

After that works, increase the rate step by step:

```text
1000 SPS -> 7500 SPS -> 15000 SPS -> 30000 SPS
```

## When To Use ADS1256 Mode

Use ADS1256 mode for:

- DC measurements
- Slow sensor logging
- Small voltage changes
- Calibrated experiments
- Low-frequency waveforms where voltage resolution matters more than time resolution

Use ESP32 native ADC mode for:

- Fast PWM shape
- Edges
- Higher time resolution
- Signals above a few kHz
