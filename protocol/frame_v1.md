# Binary Frame v1 (Template)

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
