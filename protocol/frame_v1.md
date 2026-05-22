# Binary Frame v1 (Template)

All binary frame fields are little-endian. CRC32 is calculated over the header after `magic` plus payload.

```text
magic       uint16   0xA55A
version     uint8
type        uint8    DATA / STATUS / ACK / ERROR
seq         uint32
source      uint8    0 = ESP_ADC, 1 = ADS1256
channelmask uint16
sample_hz   uint32
t0_us       uint64
dt_ns       uint32
format      uint8    U16, S24_IN_I32, F32
nsamples    uint16
payload     bytes
crc32       uint32
```

Current payload formats:

```text
U16          ESP32 ADC raw counts, one uint16 per sample
S24_IN_I32   ADS1256 sign-extended int32 per sample
F32          reserved for future converted values
```

Notes:

- `sample_hz` is the configured acquisition rate for the frame source.
- ESP32 ADC full-rate timing can be tested without streaming every sample by using the JSON command `adc_burst_test`.
- ADS1256 frames use source `1`, format `S24_IN_I32`, and one sign-extended `int32` per sample.
