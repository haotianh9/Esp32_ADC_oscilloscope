#include "adc_fast.h"

#include <stdlib.h>
#include <string.h>
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "soc/soc_caps.h"
#include "usb_stream.h"
#include "trigger.h"

static const char *TAG = "adc_fast";

#define ADC_FAST_FRAME_SAMPLES 64
#define ADC_FAST_READ_BYTES (ADC_FAST_FRAME_SAMPLES * SOC_ADC_DIGI_RESULT_BYTES)
#define ADC_FAST_BURST_FRAME_SAMPLES 256
#define ADC_FAST_BURST_READ_BYTES (ADC_FAST_BURST_FRAME_SAMPLES * SOC_ADC_DIGI_RESULT_BYTES)

static SemaphoreHandle_t s_lock;
static TaskHandle_t s_task;
static adc_continuous_handle_t s_handle;
static bool s_streaming;
static adc_fast_stats_t s_stats = {
    .in_min = UINT16_MAX,
    .out_min = UINT16_MAX,
};
static adc_fast_config_t s_config = {
    .sample_hz = 1000,
    .channel = ADC_CHANNEL_3,
    .atten = ADC_ATTEN_DB_12,
};

static esp_err_t adc_fast_start_locked(void)
{
    if (s_handle) {
        return ESP_OK;
    }

    adc_continuous_handle_cfg_t handle_cfg = {
        .max_store_buf_size = 4096,
        .conv_frame_size = ADC_FAST_READ_BYTES,
    };
    ESP_RETURN_ON_ERROR(adc_continuous_new_handle(&handle_cfg, &s_handle), TAG, "new handle");

    adc_digi_pattern_config_t pattern = {
        .atten = s_config.atten,
        .channel = s_config.channel & 0x7,
        .unit = ADC_UNIT_1,
        .bit_width = SOC_ADC_DIGI_MAX_BITWIDTH,
    };
    adc_continuous_config_t dig_cfg = {
        .sample_freq_hz = s_config.sample_hz,
        .conv_mode = ADC_CONV_SINGLE_UNIT_1,
        .pattern_num = 1,
        .adc_pattern = &pattern,
    };

    esp_err_t err = adc_continuous_config(s_handle, &dig_cfg);
    if (err == ESP_OK) {
        err = adc_continuous_start(s_handle);
    }
    if (err != ESP_OK) {
        adc_continuous_deinit(s_handle);
        s_handle = NULL;
    }
    return err;
}

static void adc_fast_stop_locked(void)
{
    if (!s_handle) {
        return;
    }
    (void)adc_continuous_stop(s_handle);
    (void)adc_continuous_deinit(s_handle);
    s_handle = NULL;
}

static int32_t adc_raw_to_mv(uint16_t raw)
{
    return (int32_t)(((uint32_t)raw * 3300u) / 4095u);
}

static uint16_t adc_normalize_raw(uint32_t raw)
{
    raw &= 0x0fffu;

    /*
     * On the current ESP32-S3 continuous path the DMA samples arrive as
     * 8-bit values while oneshot reads the same pin as 12-bit. Expand the
     * byte-shaped stream so the protocol and UI stay in 0..4095 raw counts.
     */
    if (raw <= 0xffu) {
        return (uint16_t)((raw << 4) | (raw >> 4));
    }
    return (uint16_t)raw;
}

static void adc_fast_task(void *arg)
{
    (void)arg;

    uint8_t read_buf[ADC_FAST_READ_BYTES] = {0};
    adc_continuous_data_t parsed[ADC_FAST_FRAME_SAMPLES] = {0};
    uint16_t samples[ADC_FAST_FRAME_SAMPLES] = {0};

    while (true) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        const bool streaming = s_streaming;
        const uint32_t sample_hz = s_config.sample_hz;
        const adc_channel_t channel = s_config.channel;
        if (!streaming || !s_handle) {
            xSemaphoreGive(s_lock);
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        uint32_t ret_num = 0;
        esp_err_t err = adc_continuous_read(s_handle, read_buf, sizeof(read_buf), &ret_num, 100);
        if (err == ESP_ERR_TIMEOUT) {
            xSemaphoreGive(s_lock);
            continue;
        }
        if (err != ESP_OK) {
            xSemaphoreGive(s_lock);
            ESP_LOGW(TAG, "read failed: %s", esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        uint32_t parsed_count = 0;
        err = adc_continuous_parse_data(s_handle, read_buf, ret_num, parsed, &parsed_count);
        xSemaphoreGive(s_lock);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "parse failed: %s", esp_err_to_name(err));
            continue;
        }

        uint16_t count = 0;
        uint16_t in_min = UINT16_MAX;
        uint16_t in_max = 0;
        uint16_t out_min = UINT16_MAX;
        uint16_t out_max = 0;
        uint64_t t0_us = (uint64_t)esp_timer_get_time();
        for (uint32_t i = 0; i < parsed_count && count < ADC_FAST_FRAME_SAMPLES; ++i) {
            if (!parsed[i].valid || parsed[i].channel != channel) {
                continue;
            }
            const uint16_t raw_in = (uint16_t)(parsed[i].raw_data & 0x0fffu);
            const uint16_t raw = adc_normalize_raw(raw_in);
            if (raw_in < in_min) {
                in_min = raw_in;
            }
            if (raw_in > in_max) {
                in_max = raw_in;
            }
            if (raw < out_min) {
                out_min = raw;
            }
            if (raw > out_max) {
                out_max = raw;
            }
            samples[count++] = raw;

            if (trigger_process_mv(adc_raw_to_mv(raw)) == TRIGGER_EVENT_HIT) {
                (void)usb_stream_send_json_line("{\"event\":\"trigger\",\"source\":\"esp_adc\"}");
            }
        }

        if (count == 0) {
            continue;
        }

        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_stats.in_min = in_min;
        s_stats.in_max = in_max;
        s_stats.out_min = out_min;
        s_stats.out_max = out_max;
        s_stats.frames++;
        xSemaphoreGive(s_lock);

        scope_frame_meta_t meta = {
            .type = SCOPE_FRAME_DATA,
            .source = SCOPE_SOURCE_ESP_ADC,
            .channelmask = (uint16_t)(1u << 4),
            .sample_hz = sample_hz,
            .t0_us = t0_us,
            .dt_ns = sample_hz ? (uint32_t)(1000000000ull / sample_hz) : 0,
            .format = SCOPE_FORMAT_U16,
            .nsamples = count,
        };
        (void)usb_stream_send_frame(&meta, (const uint8_t *)samples, count * sizeof(samples[0]));
    }
}

esp_err_t adc_fast_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_lock, ESP_ERR_NO_MEM, TAG, "lock");

    BaseType_t ok = xTaskCreate(adc_fast_task, "adc_fast", 6144, NULL, 8, &s_task);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "task");
    return ESP_OK;
}

esp_err_t adc_fast_configure(const adc_fast_config_t *config)
{
    if (!config || config->sample_hz < 611 || config->sample_hz > 83333) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool was_streaming = s_streaming;
    if (was_streaming) {
        adc_fast_stop_locked();
    }
    s_config = *config;
    esp_err_t err = ESP_OK;
    if (was_streaming) {
        err = adc_fast_start_locked();
    }
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t adc_fast_set_streaming(bool enabled)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = ESP_OK;
    if (enabled) {
        err = adc_fast_start_locked();
        if (err == ESP_OK) {
            s_streaming = true;
        }
    } else {
        s_streaming = false;
        adc_fast_stop_locked();
    }
    xSemaphoreGive(s_lock);
    return err;
}

adc_fast_config_t adc_fast_get_config(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    adc_fast_config_t cfg = s_config;
    xSemaphoreGive(s_lock);
    return cfg;
}

adc_fast_stats_t adc_fast_get_stats(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    adc_fast_stats_t stats = s_stats;
    xSemaphoreGive(s_lock);
    return stats;
}

bool adc_fast_is_streaming(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool streaming = s_streaming;
    xSemaphoreGive(s_lock);
    return streaming;
}

esp_err_t adc_fast_burst_stats(uint32_t sample_hz, uint32_t max_samples,
                               uint32_t timeout_ms, adc_fast_burst_stats_t *out)
{
    if (!out || sample_hz < 611 || sample_hz > 83333 || max_samples < 16) {
        return ESP_ERR_INVALID_ARG;
    }
    if (max_samples > 32768) {
        max_samples = 32768;
    }
    if (timeout_ms == 0) {
        timeout_ms = 1000;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_streaming || s_handle) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    adc_fast_config_t cfg = s_config;
    cfg.sample_hz = sample_hz;
    xSemaphoreGive(s_lock);

    uint16_t *samples = calloc(max_samples, sizeof(samples[0]));
    if (!samples) {
        return ESP_ERR_NO_MEM;
    }

    adc_continuous_handle_t handle = NULL;
    adc_continuous_handle_cfg_t handle_cfg = {
        .max_store_buf_size = 8192,
        .conv_frame_size = ADC_FAST_BURST_READ_BYTES,
    };
    esp_err_t err = adc_continuous_new_handle(&handle_cfg, &handle);
    if (err != ESP_OK) {
        free(samples);
        return err;
    }

    adc_digi_pattern_config_t pattern = {
        .atten = cfg.atten,
        .channel = cfg.channel & 0x7,
        .unit = ADC_UNIT_1,
        .bit_width = SOC_ADC_DIGI_MAX_BITWIDTH,
    };
    adc_continuous_config_t dig_cfg = {
        .sample_freq_hz = cfg.sample_hz,
        .conv_mode = ADC_CONV_SINGLE_UNIT_1,
        .pattern_num = 1,
        .adc_pattern = &pattern,
    };

    err = adc_continuous_config(handle, &dig_cfg);
    if (err == ESP_OK) {
        err = adc_continuous_start(handle);
    }

    uint8_t *read_buf = calloc(ADC_FAST_BURST_READ_BYTES, sizeof(read_buf[0]));
    adc_continuous_data_t *parsed = calloc(ADC_FAST_BURST_FRAME_SAMPLES, sizeof(parsed[0]));
    if (!read_buf || !parsed) {
        if (handle) {
            (void)adc_continuous_stop(handle);
            (void)adc_continuous_deinit(handle);
        }
        free(read_buf);
        free(parsed);
        free(samples);
        return ESP_ERR_NO_MEM;
    }
    uint32_t count = 0;
    uint16_t min_raw = UINT16_MAX;
    uint16_t max_raw = 0;
    uint64_t sum_raw = 0;
    const int64_t deadline_us = esp_timer_get_time() + (int64_t)timeout_ms * 1000;

    while (err == ESP_OK && count < max_samples && esp_timer_get_time() < deadline_us) {
        uint32_t ret_num = 0;
        err = adc_continuous_read(handle, read_buf, sizeof(read_buf), &ret_num, 100);
        if (err == ESP_ERR_TIMEOUT) {
            err = ESP_OK;
            continue;
        }
        if (err != ESP_OK) {
            break;
        }

        uint32_t parsed_count = 0;
        err = adc_continuous_parse_data(handle, read_buf, ret_num, parsed, &parsed_count);
        if (err != ESP_OK) {
            break;
        }

        for (uint32_t i = 0; i < parsed_count && count < max_samples; ++i) {
            if (!parsed[i].valid || parsed[i].channel != cfg.channel) {
                continue;
            }
            const uint16_t raw = adc_normalize_raw(parsed[i].raw_data);
            samples[count++] = raw;
            if (raw < min_raw) {
                min_raw = raw;
            }
            if (raw > max_raw) {
                max_raw = raw;
            }
            sum_raw += raw;
        }
    }

    if (handle) {
        (void)adc_continuous_stop(handle);
        (void)adc_continuous_deinit(handle);
    }

    if (err == ESP_OK && count < 16) {
        err = ESP_ERR_TIMEOUT;
    }
    if (err != ESP_OK) {
        free(read_buf);
        free(parsed);
        free(samples);
        return err;
    }

    const uint16_t threshold = (uint16_t)(((uint32_t)min_raw + max_raw) / 2u);
    bool prev_high = samples[0] >= threshold;
    uint32_t last_edge = 0;
    bool have_last_edge = false;
    uint32_t edges = 0;
    uint32_t interval_count = 0;
    uint32_t interval_min = UINT32_MAX;
    uint32_t interval_max = 0;
    uint64_t interval_sum = 0;

    for (uint32_t i = 1; i < count; ++i) {
        bool high = samples[i] >= threshold;
        if (high == prev_high) {
            continue;
        }
        edges++;
        if (have_last_edge) {
            const uint32_t interval = i - last_edge;
            if (interval < interval_min) {
                interval_min = interval;
            }
            if (interval > interval_max) {
                interval_max = interval;
            }
            interval_sum += interval;
            interval_count++;
        }
        last_edge = i;
        have_last_edge = true;
        prev_high = high;
    }

    memset(out, 0, sizeof(*out));
    out->sample_hz = cfg.sample_hz;
    out->samples = count;
    out->min_raw = min_raw;
    out->max_raw = max_raw;
    out->threshold_raw = threshold;
    out->avg_raw = (uint32_t)(sum_raw / count);
    out->edges = edges;
    if (interval_count > 0) {
        out->half_samples_min = interval_min;
        out->half_samples_max = interval_max;
        out->half_samples_avg_x100 = (uint32_t)((interval_sum * 100ull) / interval_count);
        out->period_us_x10 = (uint32_t)((interval_sum * 2ull * 10000000ull) /
                                        ((uint64_t)interval_count * cfg.sample_hz));
    }

    free(read_buf);
    free(parsed);
    free(samples);
    return ESP_OK;
}
