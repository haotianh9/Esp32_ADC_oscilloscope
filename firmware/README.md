# Firmware (ESP-IDF) Scaffold

Target responsibilities:

- Native ADC continuous DMA acquisition task
- ADS1256 SPI + DRDY acquisition task
- Trigger engine (edge/level/hysteresis + pre-trigger)
- USB CDC streaming task (TinyUSB)
- Command parser task (JSON control commands)

## Suggested layout

```text
firmware/
  CMakeLists.txt
  sdkconfig.defaults
  main/
    CMakeLists.txt
    app_main.c
    adc_fast.c/.h
    ads1256.c/.h
    trigger.c/.h
    usb_stream.c/.h
    command.c/.h
```

## Bring-up order

1. Native ADC single-channel stream at 40 kSPS
2. Binary packet framing over USB CDC
3. Trigger + pre/post capture
4. ADS1256 init and 1 kSPS single-channel read
5. Increase ADS1256 sample rate and add calibration
