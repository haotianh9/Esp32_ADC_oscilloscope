#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_adc/adc_continuous.h"

typedef struct {
    uint32_t sample_hz;
    adc_channel_t channel;
    adc_atten_t atten;
} adc_fast_config_t;

typedef struct {
    uint16_t in_min;
    uint16_t in_max;
    uint16_t out_min;
    uint16_t out_max;
    uint32_t frames;
} adc_fast_stats_t;

typedef struct {
    uint32_t sample_hz;
    uint32_t samples;
    uint16_t min_raw;
    uint16_t max_raw;
    uint16_t threshold_raw;
    uint32_t avg_raw;
    uint32_t edges;
    uint32_t half_samples_min;
    uint32_t half_samples_max;
    uint32_t half_samples_avg_x100;
    uint32_t period_us_x10;
} adc_fast_burst_stats_t;

esp_err_t adc_fast_init(void);
esp_err_t adc_fast_configure(const adc_fast_config_t *config);
esp_err_t adc_fast_set_streaming(bool enabled);
adc_fast_config_t adc_fast_get_config(void);
adc_fast_stats_t adc_fast_get_stats(void);
bool adc_fast_is_streaming(void);
esp_err_t adc_fast_burst_stats(uint32_t sample_hz, uint32_t max_samples,
                               uint32_t timeout_ms, adc_fast_burst_stats_t *out);
