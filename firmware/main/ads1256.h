#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

typedef struct {
    uint32_t sample_hz;
    uint32_t vref_mv;
    uint8_t pga;
    uint8_t mux;
    bool buffer_enabled;
} ads1256_config_t;

esp_err_t ads1256_init(void);
esp_err_t ads1256_configure(const ads1256_config_t *config);
esp_err_t ads1256_set_streaming(bool enabled);
ads1256_config_t ads1256_get_config(void);
bool ads1256_is_streaming(void);
esp_err_t ads1256_read_registers(uint8_t *out, size_t len);
esp_err_t ads1256_scan_single_ended(int32_t *out, size_t len);
esp_err_t ads1256_read_mux_stats(uint8_t mux, uint8_t discard, uint16_t samples,
                                 int32_t *min_code, int32_t *max_code, int32_t *avg_code);
int32_t ads1256_code_to_mv(int32_t code);
