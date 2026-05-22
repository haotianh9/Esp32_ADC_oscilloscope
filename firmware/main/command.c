#include "command.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include "adc_fast.h"
#include "ads1256.h"
#include "cJSON.h"
#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "scope_config.h"
#include "selftest_pwm.h"
#include "trigger.h"
#include "usb_stream.h"

static const char *TAG = "command";

#define COMMAND_LINE_MAX 512

static scope_source_t s_source = SCOPE_SOURCE_ESP_ADC;

static esp_err_t stop_all(void);

static const char *source_name(scope_source_t source)
{
    return source == SCOPE_SOURCE_ADS1256 ? "ads1256" : "esp_adc";
}

static esp_err_t send_ack(const char *cmd)
{
    char line[128] = {0};
    snprintf(line, sizeof(line), "{\"type\":\"ack\",\"cmd\":\"%s\"}", cmd ? cmd : "");
    return usb_stream_send_json_line(line);
}

static esp_err_t send_error(const char *cmd, const char *message, esp_err_t err)
{
    char line[192] = {0};
    snprintf(line, sizeof(line),
             "{\"type\":\"error\",\"cmd\":\"%s\",\"message\":\"%s\",\"code\":%d}",
             cmd ? cmd : "", message ? message : "error", (int)err);
    return usb_stream_send_json_line(line);
}

static bool json_bool(cJSON *root, const char *key, bool fallback)
{
    cJSON *item = cJSON_GetObjectItem(root, key);
    if (cJSON_IsBool(item)) {
        return cJSON_IsTrue(item);
    }
    return fallback;
}

static uint32_t json_u32(cJSON *root, const char *key, uint32_t fallback)
{
    cJSON *item = cJSON_GetObjectItem(root, key);
    if (cJSON_IsNumber(item) && item->valuedouble >= 0) {
        return (uint32_t)item->valuedouble;
    }
    return fallback;
}

static esp_err_t parse_source(const char *value, scope_source_t *source)
{
    if (!value || !source) {
        return ESP_ERR_INVALID_ARG;
    }
    if (strcmp(value, "esp_adc") == 0 || strcmp(value, "adc") == 0) {
        *source = SCOPE_SOURCE_ESP_ADC;
        return ESP_OK;
    }
    if (strcmp(value, "ads1256") == 0 || strcmp(value, "ads") == 0) {
        *source = SCOPE_SOURCE_ADS1256;
        return ESP_OK;
    }
    return ESP_ERR_INVALID_ARG;
}

static esp_err_t parse_ads_mux(const char *value, uint8_t *mux)
{
    if (!value || !mux) {
        return ESP_ERR_INVALID_ARG;
    }

    unsigned pos = 0;
    unsigned neg = 0;
    if (sscanf(value, "AIN%u-AINCOM", &pos) == 1) {
        neg = 8;
    } else if (sscanf(value, "AIN%u-AIN%u", &pos, &neg) != 2) {
        return ESP_ERR_INVALID_ARG;
    }

    if (pos > 8 || neg > 8) {
        return ESP_ERR_INVALID_ARG;
    }

    *mux = (uint8_t)((pos << 4) | neg);
    return ESP_OK;
}

static adc_atten_t parse_atten(cJSON *root, adc_atten_t fallback)
{
    cJSON *item = cJSON_GetObjectItem(root, "atten");
    if (!cJSON_IsString(item)) {
        return fallback;
    }
    if (strcmp(item->valuestring, "0db") == 0) {
        return ADC_ATTEN_DB_0;
    }
    if (strcmp(item->valuestring, "2.5db") == 0 || strcmp(item->valuestring, "2db") == 0) {
        return ADC_ATTEN_DB_2_5;
    }
    if (strcmp(item->valuestring, "6db") == 0) {
        return ADC_ATTEN_DB_6;
    }
    if (strcmp(item->valuestring, "12db") == 0 || strcmp(item->valuestring, "11db") == 0) {
        return ADC_ATTEN_DB_12;
    }
    return fallback;
}

static esp_err_t handle_status(void)
{
    uint32_t pwm_freq = 0;
    uint8_t pwm_duty = 0;
    bool pwm_enabled = false;
    selftest_pwm_get(&pwm_freq, &pwm_duty, &pwm_enabled);

    adc_fast_config_t adc_cfg = adc_fast_get_config();
    adc_fast_stats_t adc_stats = adc_fast_get_stats();
    ads1256_config_t ads_cfg = ads1256_get_config();
    trigger_config_t trig = trigger_get_config();
    gpio_num_t pwm_gpio = selftest_pwm_gpio();

    char line[640] = {0};
    snprintf(line, sizeof(line),
             "{\"type\":\"status\",\"fw\":\"%s\",\"source\":\"%s\","
             "\"gpio\":{\"pwm\":%d,\"adc\":%d,\"ads_drdy\":%d},"
             "\"esp_adc\":{\"streaming\":%s,\"fs\":%"PRIu32",\"gpio\":4,\"channel\":%d,\"stream_scale\":\"u8_to_u12\","
             "\"stats\":{\"in_min\":%u,\"in_max\":%u,\"out_min\":%u,\"out_max\":%u,\"frames\":%"PRIu32"}},"
             "\"ads1256\":{\"streaming\":%s,\"fs\":%"PRIu32",\"vref_mv\":%"PRIu32",\"pga\":%u,\"mux\":%u,\"buffer\":%s},"
             "\"pwm\":{\"gpio\":%d,\"freq_hz\":%"PRIu32",\"duty_percent\":%u,\"enabled\":%s},"
             "\"trigger\":{\"armed\":%s,\"edge\":\"%s\",\"level_mv\":%"PRId32",\"hyst_mv\":%"PRId32",\"pre\":%.3f,\"samples\":%"PRIu32"}}",
             SCOPE_FW_VERSION, source_name(s_source),
             selftest_pwm_level(), gpio_get_level(SCOPE_ADC_GPIO),
             gpio_get_level(SCOPE_ADS1256_DRDY),
             adc_fast_is_streaming() ? "true" : "false", adc_cfg.sample_hz, (int)adc_cfg.channel,
             adc_stats.in_min == UINT16_MAX ? 0 : adc_stats.in_min, adc_stats.in_max,
             adc_stats.out_min == UINT16_MAX ? 0 : adc_stats.out_min, adc_stats.out_max,
             adc_stats.frames,
             ads1256_is_streaming() ? "true" : "false", ads_cfg.sample_hz, ads_cfg.vref_mv,
             ads_cfg.pga, ads_cfg.mux,
             ads_cfg.buffer_enabled ? "true" : "false",
             (int)pwm_gpio, pwm_freq, pwm_duty, pwm_enabled ? "true" : "false",
             trig.armed ? "true" : "false",
             trig.edge == TRIGGER_EDGE_FALLING ? "falling" : "rising",
             trig.level_mv, trig.hysteresis_mv, trig.pre_trigger, trig.samples);
    return usb_stream_send_json_line(line);
}

static esp_err_t handle_adc_probe(void)
{
    if (adc_fast_is_streaming()) {
        return send_error("adc_probe", "stop stream before probing", ESP_ERR_INVALID_STATE);
    }

    adc_fast_config_t cfg = adc_fast_get_config();
    adc_oneshot_unit_handle_t handle = NULL;
    adc_oneshot_unit_init_cfg_t unit_cfg = {
        .unit_id = ADC_UNIT_1,
        .clk_src = ADC_RTC_CLK_SRC_DEFAULT,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    ESP_RETURN_ON_ERROR(adc_oneshot_new_unit(&unit_cfg, &handle), TAG, "adc probe unit");

    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten = cfg.atten,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    esp_err_t err = adc_oneshot_config_channel(handle, cfg.channel, &chan_cfg);

    int min_raw = 4095;
    int max_raw = 0;
    int64_t sum_raw = 0;
    int samples = 0;
    if (err == ESP_OK) {
        for (int i = 0; i < 32; ++i) {
            int raw = 0;
            err = adc_oneshot_read(handle, cfg.channel, &raw);
            if (err != ESP_OK) {
                break;
            }
            if (raw < min_raw) {
                min_raw = raw;
            }
            if (raw > max_raw) {
                max_raw = raw;
            }
            sum_raw += raw;
            samples++;
        }
    }

    (void)adc_oneshot_del_unit(handle);
    if (err != ESP_OK || samples == 0) {
        return send_error("adc_probe", "adc oneshot read failed", err);
    }

    char line[192] = {0};
    snprintf(line, sizeof(line),
             "{\"type\":\"adc_probe\",\"gpio\":4,\"channel\":%d,\"samples\":%d,"
             "\"min\":%d,\"max\":%d,\"avg\":%d,\"gpio_level\":%d}",
             (int)cfg.channel, samples, min_raw, max_raw, (int)(sum_raw / samples),
             gpio_get_level(SCOPE_ADC_GPIO));
    return usb_stream_send_json_line(line);
}

static esp_err_t handle_adc_burst_test(cJSON *root)
{
    const uint32_t sample_hz = json_u32(root, "fs", adc_fast_get_config().sample_hz);
    const uint32_t samples = json_u32(root, "samples", 8192);
    const uint32_t timeout_ms = json_u32(root, "timeout_ms", 1000);
    const uint32_t pwm_hz = json_u32(root, "pwm_hz", 0);
    const uint8_t pwm_duty = (uint8_t)json_u32(root, "duty_percent", 50);

    ESP_RETURN_ON_ERROR(stop_all(), TAG, "stop before adc burst");
    if (pwm_hz > 0) {
        ESP_RETURN_ON_ERROR(selftest_pwm_configure(pwm_hz, pwm_duty, true), TAG, "burst pwm");
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    adc_fast_burst_stats_t stats = {0};
    esp_err_t err = adc_fast_burst_stats(sample_hz, samples, timeout_ms, &stats);
    if (err != ESP_OK) {
        return send_error("adc_burst_test", "burst failed", err);
    }

    uint32_t expected_period_samples_x100 = 0;
    if (pwm_hz > 0) {
        expected_period_samples_x100 = (uint32_t)(((uint64_t)stats.sample_hz * 100ull) / pwm_hz);
    }

    char line[512] = {0};
    snprintf(line, sizeof(line),
             "{\"type\":\"adc_burst_test\",\"fs\":%"PRIu32",\"samples\":%"PRIu32",\"pwm_hz\":%"PRIu32","
             "\"min\":%u,\"max\":%u,\"avg\":%"PRIu32",\"pkpk\":%u,\"threshold\":%u,"
             "\"edges\":%"PRIu32",\"half_samples_avg_x100\":%"PRIu32","
             "\"half_samples_min\":%"PRIu32",\"half_samples_max\":%"PRIu32","
             "\"period_us_x10\":%"PRIu32",\"expected_period_samples_x100\":%"PRIu32"}",
             stats.sample_hz, stats.samples, pwm_hz,
             stats.min_raw, stats.max_raw, stats.avg_raw,
             (unsigned)(stats.max_raw - stats.min_raw), stats.threshold_raw,
             stats.edges, stats.half_samples_avg_x100,
             stats.half_samples_min, stats.half_samples_max,
             stats.period_us_x10, expected_period_samples_x100);
    return usb_stream_send_json_line(line);
}

static esp_err_t handle_pwm_adc_probe(cJSON *root)
{
    (void)root;
    return send_error("pwm_adc_probe", "unsupported: ADC2 reconfigures gpio18 while reading", ESP_ERR_NOT_SUPPORTED);
}

static esp_err_t stop_all(void)
{
    trigger_stop();
    esp_err_t err_adc = adc_fast_set_streaming(false);
    esp_err_t err_ads = ads1256_set_streaming(false);
    return err_adc != ESP_OK ? err_adc : err_ads;
}

static esp_err_t handle_pwm_ads_probe(cJSON *root)
{
    uint32_t old_freq = 0;
    uint8_t old_duty = 0;
    bool old_enabled = false;
    selftest_pwm_get(&old_freq, &old_duty, &old_enabled);
    gpio_num_t old_gpio = selftest_pwm_gpio();
    gpio_num_t probe_gpio = old_gpio;
    cJSON *gpio_item = cJSON_GetObjectItem(root, "gpio");
    if (cJSON_IsNumber(gpio_item) && gpio_item->valuedouble >= 0) {
        probe_gpio = (gpio_num_t)gpio_item->valueint;
    }

    ads1256_config_t ads_cfg = ads1256_get_config();
    uint8_t mux = ads_cfg.mux;
    cJSON *channel = cJSON_GetObjectItem(root, "channel");
    if (cJSON_IsString(channel)) {
        esp_err_t mux_err = parse_ads_mux(channel->valuestring, &mux);
        if (mux_err != ESP_OK) {
            return send_error("pwm_ads_probe", "bad ads1256 channel", mux_err);
        }
    }

    const uint32_t settle_ms = json_u32(root, "settle_ms", 100);
    const uint16_t samples = (uint16_t)json_u32(root, "samples", 8);
    const uint8_t discard = (uint8_t)json_u32(root, "discard", 3);

    ESP_RETURN_ON_ERROR(stop_all(), TAG, "stop before pwm probe");

    int32_t low_min = 0;
    int32_t low_max = 0;
    int32_t low_avg = 0;
    int32_t high_min = 0;
    int32_t high_max = 0;
    int32_t high_avg = 0;

    esp_err_t err = selftest_pwm_configure_pin(probe_gpio, 1000, 0, true);
    if (err == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(settle_ms));
        err = ads1256_read_mux_stats(mux, discard, samples, &low_min, &low_max, &low_avg);
    }
    int low_gpio_level = selftest_pwm_level();

    if (err == ESP_OK) {
        err = selftest_pwm_configure_pin(probe_gpio, 1000, 100, true);
    }
    if (err == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(settle_ms));
        err = ads1256_read_mux_stats(mux, discard, samples, &high_min, &high_max, &high_avg);
    }
    int high_gpio_level = selftest_pwm_level();

    (void)selftest_pwm_configure_pin(old_gpio, old_freq ? old_freq : 50, old_duty, old_enabled);

    if (err != ESP_OK) {
        return send_error("pwm_ads_probe", "probe failed", err);
    }

    const int32_t low_mv = ads1256_code_to_mv(low_avg);
    const int32_t high_mv = ads1256_code_to_mv(high_avg);
    char line[512] = {0};
    snprintf(line, sizeof(line),
             "{\"type\":\"pwm_ads_probe\",\"gpio\":%d,\"mux\":%u,\"vref_mv\":%"PRIu32",\"pga\":%u,"
             "\"low\":{\"code_avg\":%"PRId32",\"code_min\":%"PRId32",\"code_max\":%"PRId32",\"mv\":%"PRId32",\"gpio\":%d},"
             "\"high\":{\"code_avg\":%"PRId32",\"code_min\":%"PRId32",\"code_max\":%"PRId32",\"mv\":%"PRId32",\"gpio\":%d},"
             "\"delta_mv\":%"PRId32"}",
             (int)probe_gpio, mux, ads_cfg.vref_mv, ads_cfg.pga,
             low_avg, low_min, low_max, low_mv, low_gpio_level,
             high_avg, high_min, high_max, high_mv, high_gpio_level,
             high_mv - low_mv);
    return usb_stream_send_json_line(line);
}

static esp_err_t start_current_source(void)
{
    if (s_source == SCOPE_SOURCE_ESP_ADC) {
        ESP_RETURN_ON_ERROR(ads1256_set_streaming(false), TAG, "stop ads");
        return adc_fast_set_streaming(true);
    }
    ESP_RETURN_ON_ERROR(adc_fast_set_streaming(false), TAG, "stop adc");
    return ads1256_set_streaming(true);
}

static esp_err_t handle_command(cJSON *root)
{
    cJSON *cmd_item = cJSON_GetObjectItem(root, "cmd");
    if (!cJSON_IsString(cmd_item)) {
        return send_error("", "missing cmd", ESP_ERR_INVALID_ARG);
    }
    const char *cmd = cmd_item->valuestring;

    if (strcmp(cmd, "set_pwm") == 0) {
        const uint32_t freq = json_u32(root, "freq_hz", json_u32(root, "freq", 1000));
        const uint8_t duty = (uint8_t)json_u32(root, "duty_percent", json_u32(root, "duty", 50));
        const bool enabled = json_bool(root, "enabled", true);
        gpio_num_t gpio = selftest_pwm_gpio();
        cJSON *gpio_item = cJSON_GetObjectItem(root, "gpio");
        if (cJSON_IsNumber(gpio_item) && gpio_item->valuedouble >= 0) {
            gpio = (gpio_num_t)gpio_item->valueint;
        }
        esp_err_t err = selftest_pwm_configure_pin(gpio, freq, duty, enabled);
        return err == ESP_OK ? send_ack(cmd) : send_error(cmd, "bad pwm config", err);
    }

    if (strcmp(cmd, "set_source") == 0) {
        cJSON *source_item = cJSON_GetObjectItem(root, "source");
        scope_source_t source = s_source;
        esp_err_t err = parse_source(cJSON_IsString(source_item) ? source_item->valuestring : NULL, &source);
        if (err != ESP_OK) {
            return send_error(cmd, "unknown source", err);
        }
        ESP_RETURN_ON_ERROR(stop_all(), TAG, "stop before source switch");
        s_source = source;
        return send_ack(cmd);
    }

    if (strcmp(cmd, "set_adc") == 0) {
        adc_fast_config_t cfg = adc_fast_get_config();
        cfg.sample_hz = json_u32(root, "fs", cfg.sample_hz);
        cfg.channel = ADC_CHANNEL_3;
        cfg.atten = parse_atten(root, cfg.atten);
        esp_err_t err = adc_fast_configure(&cfg);
        return err == ESP_OK ? send_ack(cmd) : send_error(cmd, "bad adc config", err);
    }

    if (strcmp(cmd, "set_ads1256") == 0) {
        ads1256_config_t cfg = ads1256_get_config();
        cfg.sample_hz = json_u32(root, "fs", cfg.sample_hz);
        cfg.vref_mv = json_u32(root, "vref_mv", cfg.vref_mv);
        cfg.pga = (uint8_t)json_u32(root, "pga", cfg.pga);
        cfg.buffer_enabled = json_bool(root, "buffer", cfg.buffer_enabled);
        cJSON *channel = cJSON_GetObjectItem(root, "channel");
        if (cJSON_IsString(channel)) {
            esp_err_t mux_err = parse_ads_mux(channel->valuestring, &cfg.mux);
            if (mux_err != ESP_OK) {
                return send_error(cmd, "bad ads1256 channel", mux_err);
            }
        }
        esp_err_t err = ads1256_configure(&cfg);
        return err == ESP_OK ? send_ack(cmd) : send_error(cmd, "bad ads1256 config", err);
    }

    if (strcmp(cmd, "stream") == 0) {
        const bool enabled = json_bool(root, "enabled", true);
        esp_err_t err = enabled ? start_current_source() : stop_all();
        return err == ESP_OK ? send_ack(cmd) : send_error(cmd, "stream failed", err);
    }

    if (strcmp(cmd, "arm") == 0) {
        trigger_config_t cfg = trigger_get_config();
        cfg.armed = true;
        cJSON *edge = cJSON_GetObjectItem(root, "edge");
        if (cJSON_IsString(edge) && strcmp(edge->valuestring, "falling") == 0) {
            cfg.edge = TRIGGER_EDGE_FALLING;
        } else {
            cfg.edge = TRIGGER_EDGE_RISING;
        }
        cfg.level_mv = (int32_t)json_u32(root, "level_mv", (uint32_t)cfg.level_mv);
        cfg.hysteresis_mv = (int32_t)json_u32(root, "hyst_mv", (uint32_t)cfg.hysteresis_mv);
        cJSON *pre = cJSON_GetObjectItem(root, "pre");
        if (cJSON_IsNumber(pre)) {
            cfg.pre_trigger = (float)pre->valuedouble;
        }
        cfg.samples = json_u32(root, "samples", cfg.samples);
        esp_err_t err = trigger_configure(&cfg);
        return err == ESP_OK ? send_ack(cmd) : send_error(cmd, "bad trigger config", err);
    }

    if (strcmp(cmd, "stop") == 0) {
        esp_err_t err = stop_all();
        return err == ESP_OK ? send_ack(cmd) : send_error(cmd, "stop failed", err);
    }

    if (strcmp(cmd, "status") == 0) {
        return handle_status();
    }

    if (strcmp(cmd, "adc_probe") == 0) {
        return handle_adc_probe();
    }

    if (strcmp(cmd, "adc_burst_test") == 0) {
        return handle_adc_burst_test(root);
    }

    if (strcmp(cmd, "pwm_adc_probe") == 0) {
        return handle_pwm_adc_probe(root);
    }

    if (strcmp(cmd, "pwm_ads_probe") == 0) {
        return handle_pwm_ads_probe(root);
    }

    if (strcmp(cmd, "ads1256_regs") == 0) {
        uint8_t regs[4] = {0};
        esp_err_t err = ads1256_read_registers(regs, sizeof(regs));
        if (err != ESP_OK) {
            return send_error(cmd, "ads1256 register read failed", err);
        }
        char line[160] = {0};
        snprintf(line, sizeof(line),
                 "{\"type\":\"ads1256_regs\",\"status\":%u,\"mux\":%u,\"adcon\":%u,\"drate\":%u}",
                 regs[0], regs[1], regs[2], regs[3]);
        return usb_stream_send_json_line(line);
    }

    if (strcmp(cmd, "ads1256_scan") == 0) {
        int32_t codes[8] = {0};
        esp_err_t err = ads1256_scan_single_ended(codes, sizeof(codes) / sizeof(codes[0]));
        if (err != ESP_OK) {
            return send_error(cmd, "ads1256 scan failed", err);
        }
        char line[640] = {0};
        snprintf(line, sizeof(line),
                 "{\"type\":\"ads1256_scan\",\"channels\":["
                 "{\"ch\":0,\"code\":%"PRId32",\"mv\":%"PRId32"},"
                 "{\"ch\":1,\"code\":%"PRId32",\"mv\":%"PRId32"},"
                 "{\"ch\":2,\"code\":%"PRId32",\"mv\":%"PRId32"},"
                 "{\"ch\":3,\"code\":%"PRId32",\"mv\":%"PRId32"},"
                 "{\"ch\":4,\"code\":%"PRId32",\"mv\":%"PRId32"},"
                 "{\"ch\":5,\"code\":%"PRId32",\"mv\":%"PRId32"},"
                 "{\"ch\":6,\"code\":%"PRId32",\"mv\":%"PRId32"},"
                 "{\"ch\":7,\"code\":%"PRId32",\"mv\":%"PRId32"}]}",
                 codes[0], ads1256_code_to_mv(codes[0]),
                 codes[1], ads1256_code_to_mv(codes[1]),
                 codes[2], ads1256_code_to_mv(codes[2]),
                 codes[3], ads1256_code_to_mv(codes[3]),
                 codes[4], ads1256_code_to_mv(codes[4]),
                 codes[5], ads1256_code_to_mv(codes[5]),
                 codes[6], ads1256_code_to_mv(codes[6]),
                 codes[7], ads1256_code_to_mv(codes[7]));
        return usb_stream_send_json_line(line);
    }

    return send_error(cmd, "unknown command", ESP_ERR_NOT_SUPPORTED);
}

static void command_task(void *arg)
{
    (void)arg;
    uint8_t chunk[CONFIG_TINYUSB_CDC_RX_BUFSIZE] = {0};
    char line[COMMAND_LINE_MAX] = {0};
    size_t line_len = 0;

    while (true) {
        const size_t n = usb_stream_read_rx(chunk, sizeof(chunk), 250);
        if (n == 0) {
            continue;
        }

        for (size_t i = 0; i < n; ++i) {
            const char c = (char)chunk[i];
            if (c == '\r') {
                continue;
            }
            if (c == '\n') {
                line[line_len] = '\0';
                if (line_len > 0) {
                    cJSON *root = cJSON_Parse(line);
                    if (root) {
                        (void)handle_command(root);
                        cJSON_Delete(root);
                    } else {
                        (void)send_error("", "invalid json", ESP_ERR_INVALID_ARG);
                    }
                }
                line_len = 0;
                continue;
            }
            if (line_len + 1 < sizeof(line)) {
                line[line_len++] = c;
            } else {
                line_len = 0;
                (void)send_error("", "command too long", ESP_ERR_NO_MEM);
            }
        }
    }
}

esp_err_t command_init(void)
{
    BaseType_t ok = xTaskCreate(command_task, "command", 6144, NULL, 6, NULL);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "task");
    return ESP_OK;
}
