#include "ads1256.h"

#include <string.h>
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "scope_config.h"
#include "trigger.h"
#include "usb_stream.h"

static const char *TAG = "ads1256";

#define ADS1256_CMD_WAKEUP 0x00
#define ADS1256_CMD_RDATA 0x01
#define ADS1256_CMD_RREG 0x10
#define ADS1256_CMD_WREG 0x50
#define ADS1256_CMD_SYNC 0xfc
#define ADS1256_CMD_RESET 0xfe
#define ADS1256_CMD_SELFCAL 0xf0

#define ADS1256_REG_STATUS 0x00
#define ADS1256_REG_MUX 0x01
#define ADS1256_REG_ADCON 0x02
#define ADS1256_REG_DRATE 0x03

#define ADS1256_FRAME_SAMPLES 64

typedef struct {
    uint32_t requested_hz;
    uint32_t actual_hz;
    uint8_t drate;
} ads1256_rate_t;

static const ads1256_rate_t RATE_TABLE[] = {
    {30000, 30000, 0xf0},
    {15000, 15000, 0xe0},
    {7500, 7500, 0xd0},
    {3750, 3750, 0xc0},
    {2000, 2000, 0xb0},
    {1000, 1000, 0xa1},
    {500, 500, 0x92},
    {100, 100, 0x82},
    {60, 60, 0x72},
    {50, 50, 0x63},
    {30, 30, 0x53},
    {25, 25, 0x43},
    {15, 15, 0x33},
    {10, 10, 0x23},
    {5, 5, 0x13},
    {3, 3, 0x03},
};

static SemaphoreHandle_t s_lock;
static SemaphoreHandle_t s_drdy_sem;
static TaskHandle_t s_task;
static spi_device_handle_t s_spi;
static bool s_bus_ready;
static bool s_device_ready;
static bool s_streaming;
static ads1256_config_t s_config = {
    .sample_hz = 1000,
    .vref_mv = 2500,
    .pga = 1,
    .mux = 0x08,
    .buffer_enabled = false,
};
static uint32_t s_actual_hz = 1000;
static uint8_t s_drate = 0xa1;

static esp_err_t ads1256_apply_config_locked(void);
static void drain_drdy_sem(void);

static void IRAM_ATTR drdy_isr(void *arg)
{
    (void)arg;
    BaseType_t higher_woken = pdFALSE;
    xSemaphoreGiveFromISR(s_drdy_sem, &higher_woken);
    if (higher_woken == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}

static esp_err_t ads1256_select(void)
{
    return gpio_set_level(SCOPE_ADS1256_CS, 0);
}

static esp_err_t ads1256_deselect(void)
{
    return gpio_set_level(SCOPE_ADS1256_CS, 1);
}

static void drain_drdy_sem(void)
{
    while (xSemaphoreTake(s_drdy_sem, 0) == pdTRUE) {
    }
}

static esp_err_t spi_transfer(const uint8_t *tx, uint8_t *rx, size_t len)
{
    spi_transaction_t transaction = {
        .length = len * 8,
        .tx_buffer = tx,
        .rx_buffer = rx,
    };
    return spi_device_transmit(s_spi, &transaction);
}

static esp_err_t spi_write_byte(uint8_t value)
{
    return spi_transfer(&value, NULL, 1);
}

static esp_err_t spi_read_bytes(uint8_t *out, size_t len)
{
    uint8_t zeros[16] = {0};
    if (len > sizeof(zeros)) {
        return ESP_ERR_INVALID_ARG;
    }
    return spi_transfer(zeros, out, len);
}

static bool rate_from_hz(uint32_t requested_hz, uint32_t *actual_hz, uint8_t *drate)
{
    for (size_t i = 0; i < sizeof(RATE_TABLE) / sizeof(RATE_TABLE[0]); ++i) {
        if (requested_hz >= RATE_TABLE[i].requested_hz) {
            *actual_hz = RATE_TABLE[i].actual_hz;
            *drate = RATE_TABLE[i].drate;
            return true;
        }
    }
    *actual_hz = 3;
    *drate = 0x03;
    return true;
}

static bool pga_to_bits(uint8_t pga, uint8_t *bits)
{
    switch (pga) {
    case 1: *bits = 0; return true;
    case 2: *bits = 1; return true;
    case 4: *bits = 2; return true;
    case 8: *bits = 3; return true;
    case 16: *bits = 4; return true;
    case 32: *bits = 5; return true;
    case 64: *bits = 6; return true;
    default: return false;
    }
}

static esp_err_t write_reg(uint8_t reg, uint8_t value)
{
    ESP_RETURN_ON_ERROR(ads1256_select(), TAG, "cs low");
    ESP_RETURN_ON_ERROR(spi_write_byte(ADS1256_CMD_WREG | reg), TAG, "wreg");
    ESP_RETURN_ON_ERROR(spi_write_byte(0x00), TAG, "wreg count");
    ESP_RETURN_ON_ERROR(spi_write_byte(value), TAG, "wreg value");
    esp_rom_delay_us(4);
    ESP_RETURN_ON_ERROR(ads1256_deselect(), TAG, "cs high");
    return ESP_OK;
}

static esp_err_t read_regs(uint8_t reg, uint8_t *out, size_t len)
{
    if (!out || len == 0 || len > 16) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_RETURN_ON_ERROR(ads1256_select(), TAG, "cs low");
    ESP_RETURN_ON_ERROR(spi_write_byte(ADS1256_CMD_RREG | reg), TAG, "rreg");
    ESP_RETURN_ON_ERROR(spi_write_byte((uint8_t)(len - 1)), TAG, "rreg count");
    esp_rom_delay_us(8);
    ESP_RETURN_ON_ERROR(spi_read_bytes(out, len), TAG, "rreg data");
    ESP_RETURN_ON_ERROR(ads1256_deselect(), TAG, "cs high");
    return ESP_OK;
}

static esp_err_t ads1256_hw_init_locked(void)
{
    if (s_device_ready) {
        return ESP_OK;
    }

    gpio_config_t output_cfg = {
        .pin_bit_mask = (1ull << SCOPE_ADS1256_CS) |
                        (1ull << SCOPE_ADS1256_RESET) |
                        (1ull << SCOPE_ADS1256_SYNC_PDWN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&output_cfg), TAG, "output gpio");
    gpio_set_level(SCOPE_ADS1256_CS, 1);
    gpio_set_level(SCOPE_ADS1256_SYNC_PDWN, 1);

    gpio_config_t input_cfg = {
        .pin_bit_mask = (1ull << SCOPE_ADS1256_DRDY),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_NEGEDGE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&input_cfg), TAG, "drdy gpio");

    esp_err_t isr_err = gpio_install_isr_service(0);
    if (isr_err != ESP_OK && isr_err != ESP_ERR_INVALID_STATE) {
        return isr_err;
    }
    ESP_RETURN_ON_ERROR(gpio_isr_handler_add(SCOPE_ADS1256_DRDY, drdy_isr, NULL), TAG, "drdy isr");

    if (!s_bus_ready) {
        spi_bus_config_t bus_cfg = {
            .mosi_io_num = SCOPE_ADS1256_MOSI,
            .miso_io_num = SCOPE_ADS1256_MISO,
            .sclk_io_num = SCOPE_ADS1256_SCLK,
            .quadwp_io_num = -1,
            .quadhd_io_num = -1,
            .max_transfer_sz = 16,
        };
        esp_err_t err = spi_bus_initialize(SPI2_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            return err;
        }
        s_bus_ready = true;
    }

    spi_device_interface_config_t dev_cfg = {
        .clock_speed_hz = 1000000,
        .mode = 1,
        .spics_io_num = -1,
        .queue_size = 1,
    };
    ESP_RETURN_ON_ERROR(spi_bus_add_device(SPI2_HOST, &dev_cfg, &s_spi), TAG, "spi device");

    gpio_set_level(SCOPE_ADS1256_RESET, 0);
    vTaskDelay(pdMS_TO_TICKS(2));
    gpio_set_level(SCOPE_ADS1256_RESET, 1);
    vTaskDelay(pdMS_TO_TICKS(10));

    ESP_RETURN_ON_ERROR(ads1256_select(), TAG, "cs low");
    ESP_RETURN_ON_ERROR(spi_write_byte(ADS1256_CMD_RESET), TAG, "reset");
    ESP_RETURN_ON_ERROR(ads1256_deselect(), TAG, "cs high");
    vTaskDelay(pdMS_TO_TICKS(5));

    s_device_ready = true;
    return ads1256_apply_config_locked();
}

static esp_err_t ads1256_apply_config_locked(void)
{
    uint8_t pga_bits = 0;
    if (!pga_to_bits(s_config.pga, &pga_bits)) {
        return ESP_ERR_INVALID_ARG;
    }
    rate_from_hz(s_config.sample_hz, &s_actual_hz, &s_drate);

    const uint8_t status = s_config.buffer_enabled ? 0x03 : 0x01;
    const uint8_t mux = s_config.mux;
    const uint8_t adcon = pga_bits;

    ESP_RETURN_ON_ERROR(write_reg(ADS1256_REG_STATUS, status), TAG, "status");
    ESP_RETURN_ON_ERROR(write_reg(ADS1256_REG_MUX, mux), TAG, "mux");
    ESP_RETURN_ON_ERROR(write_reg(ADS1256_REG_ADCON, adcon), TAG, "adcon");
    ESP_RETURN_ON_ERROR(write_reg(ADS1256_REG_DRATE, s_drate), TAG, "drate");

    ESP_RETURN_ON_ERROR(ads1256_select(), TAG, "cs low");
    ESP_RETURN_ON_ERROR(spi_write_byte(ADS1256_CMD_SELFCAL), TAG, "selfcal");
    ESP_RETURN_ON_ERROR(ads1256_deselect(), TAG, "cs high");
    vTaskDelay(pdMS_TO_TICKS(400));
    return ESP_OK;
}

static esp_err_t wait_fresh_drdy(uint32_t timeout_ms)
{
    const int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;

    while (gpio_get_level(SCOPE_ADS1256_DRDY) == 0) {
        if (esp_timer_get_time() >= deadline) {
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    drain_drdy_sem();
    while (gpio_get_level(SCOPE_ADS1256_DRDY) != 0) {
        if (esp_timer_get_time() >= deadline) {
            return ESP_ERR_TIMEOUT;
        }
        const int64_t remain_us = deadline - esp_timer_get_time();
        uint32_t wait_ms = (uint32_t)((remain_us + 999) / 1000);
        if (wait_ms == 0) {
            wait_ms = 1;
        }
        if (wait_ms > 20) {
            wait_ms = 20;
        }
        (void)xSemaphoreTake(s_drdy_sem, pdMS_TO_TICKS(wait_ms));
    }

    return ESP_OK;
}

static uint32_t fresh_sample_timeout_ms(void)
{
    const uint32_t hz = s_actual_hz > 0 ? s_actual_hz : 1000;
    uint32_t timeout_ms = (uint32_t)((4000ull + hz - 1) / hz);
    if (timeout_ms < 20) {
        timeout_ms = 20;
    }
    if (timeout_ms > 1000) {
        timeout_ms = 1000;
    }
    return timeout_ms;
}

static esp_err_t restart_conversion(void)
{
    ESP_RETURN_ON_ERROR(ads1256_select(), TAG, "cs low");
    ESP_RETURN_ON_ERROR(spi_write_byte(ADS1256_CMD_SYNC), TAG, "sync");
    esp_rom_delay_us(4);
    ESP_RETURN_ON_ERROR(spi_write_byte(ADS1256_CMD_WAKEUP), TAG, "wakeup");
    ESP_RETURN_ON_ERROR(ads1256_deselect(), TAG, "cs high");
    return ESP_OK;
}

static esp_err_t read_sample(int32_t *out)
{
    uint8_t bytes[3] = {0};

    ESP_RETURN_ON_ERROR(ads1256_select(), TAG, "cs low");
    ESP_RETURN_ON_ERROR(spi_write_byte(ADS1256_CMD_RDATA), TAG, "rdata");
    esp_rom_delay_us(8);
    ESP_RETURN_ON_ERROR(spi_read_bytes(bytes, sizeof(bytes)), TAG, "sample");
    ESP_RETURN_ON_ERROR(ads1256_deselect(), TAG, "cs high");

    int32_t code = ((int32_t)bytes[0] << 16) | ((int32_t)bytes[1] << 8) | bytes[2];
    if (code & 0x800000) {
        code |= 0xff000000;
    }
    *out = code;
    return ESP_OK;
}

static esp_err_t discard_ready_sample_locked(void)
{
    if (gpio_get_level(SCOPE_ADS1256_DRDY) != 0) {
        return ESP_OK;
    }

    int32_t unused = 0;
    return read_sample(&unused);
}

static esp_err_t read_fresh_sample_locked(int32_t *out)
{
    ESP_RETURN_ON_ERROR(discard_ready_sample_locked(), TAG, "discard stale");
    drain_drdy_sem();
    ESP_RETURN_ON_ERROR(restart_conversion(), TAG, "restart");
    ESP_RETURN_ON_ERROR(wait_fresh_drdy(fresh_sample_timeout_ms()), TAG, "fresh drdy");
    return read_sample(out);
}

static void ads1256_task(void *arg)
{
    (void)arg;
    int32_t samples[ADS1256_FRAME_SAMPLES] = {0};
    uint16_t count = 0;
    uint64_t t0_us = 0;

    while (true) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        const bool streaming = s_streaming;
        const uint32_t sample_hz = s_actual_hz;
        xSemaphoreGive(s_lock);

        if (!streaming) {
            count = 0;
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        if (wait_fresh_drdy(250) != ESP_OK) {
            continue;
        }

        int32_t sample = 0;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        esp_err_t err = s_device_ready ? read_sample(&sample) : ESP_ERR_INVALID_STATE;
        xSemaphoreGive(s_lock);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "sample read failed: %s", esp_err_to_name(err));
            continue;
        }

        if (count == 0) {
            t0_us = (uint64_t)esp_timer_get_time();
        }
        samples[count++] = sample;

        if (trigger_process_mv(ads1256_code_to_mv(sample)) == TRIGGER_EVENT_HIT) {
            (void)usb_stream_send_json_line("{\"event\":\"trigger\",\"source\":\"ads1256\"}");
        }

        if (count >= ADS1256_FRAME_SAMPLES) {
            scope_frame_meta_t meta = {
                .type = SCOPE_FRAME_DATA,
                .source = SCOPE_SOURCE_ADS1256,
                .channelmask = 0x0001,
                .sample_hz = sample_hz,
                .t0_us = t0_us,
                .dt_ns = sample_hz ? (uint32_t)(1000000000ull / sample_hz) : 0,
                .format = SCOPE_FORMAT_S24_IN_I32,
                .nsamples = count,
            };
            (void)usb_stream_send_frame(&meta, (const uint8_t *)samples, count * sizeof(samples[0]));
            count = 0;
        }
    }
}

esp_err_t ads1256_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_drdy_sem = xSemaphoreCreateBinary();
    ESP_RETURN_ON_FALSE(s_lock && s_drdy_sem, ESP_ERR_NO_MEM, TAG, "sync");

    BaseType_t ok = xTaskCreate(ads1256_task, "ads1256", 6144, NULL, 7, &s_task);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "task");
    return ESP_OK;
}

esp_err_t ads1256_configure(const ads1256_config_t *config)
{
    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t pga_bits = 0;
    if (!pga_to_bits(config->pga, &pga_bits)) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_config = *config;
    rate_from_hz(s_config.sample_hz, &s_actual_hz, &s_drate);
    esp_err_t err = s_device_ready ? ads1256_apply_config_locked() : ESP_OK;
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t ads1256_set_streaming(bool enabled)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = ESP_OK;
    if (enabled) {
        err = ads1256_hw_init_locked();
        if (err == ESP_OK) {
            (void)discard_ready_sample_locked();
            drain_drdy_sem();
            (void)restart_conversion();
            s_streaming = true;
        }
    } else {
        s_streaming = false;
    }
    xSemaphoreGive(s_lock);
    return err;
}

ads1256_config_t ads1256_get_config(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    ads1256_config_t cfg = s_config;
    xSemaphoreGive(s_lock);
    return cfg;
}

bool ads1256_is_streaming(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool streaming = s_streaming;
    xSemaphoreGive(s_lock);
    return streaming;
}

esp_err_t ads1256_read_registers(uint8_t *out, size_t len)
{
    if (!out || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = ads1256_hw_init_locked();
    if (err == ESP_OK) {
        err = read_regs(ADS1256_REG_STATUS, out, len);
    }
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t ads1256_scan_single_ended(int32_t *out, size_t len)
{
    if (!out || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (len > 8) {
        len = 8;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_streaming) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = ads1256_hw_init_locked();
    for (size_t ch = 0; err == ESP_OK && ch < len; ++ch) {
        err = write_reg(ADS1256_REG_MUX, (uint8_t)((ch << 4) | 0x08));
        if (err != ESP_OK) {
            break;
        }
        int32_t sample = 0;
        err = read_fresh_sample_locked(&sample);
        if (err == ESP_OK) {
            err = read_fresh_sample_locked(&sample);
        }
        out[ch] = sample;
    }

    (void)write_reg(ADS1256_REG_MUX, s_config.mux);
    (void)restart_conversion();
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t ads1256_read_mux_stats(uint8_t mux, uint8_t discard, uint16_t samples,
                                 int32_t *min_code, int32_t *max_code, int32_t *avg_code)
{
    if (!min_code || !max_code || !avg_code || samples == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_streaming) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = ads1256_hw_init_locked();
    if (err == ESP_OK) {
        err = write_reg(ADS1256_REG_MUX, mux);
    }

    int32_t min_sample = INT32_MAX;
    int32_t max_sample = INT32_MIN;
    int64_t sum = 0;
    const uint32_t total = (uint32_t)discard + samples;

    for (uint32_t i = 0; err == ESP_OK && i < total; ++i) {
        int32_t sample = 0;
        err = read_fresh_sample_locked(&sample);
        if (err != ESP_OK) {
            break;
        }
        if (i < discard) {
            continue;
        }
        if (sample < min_sample) {
            min_sample = sample;
        }
        if (sample > max_sample) {
            max_sample = sample;
        }
        sum += sample;
    }

    (void)write_reg(ADS1256_REG_MUX, s_config.mux);
    (void)restart_conversion();
    if (err == ESP_OK) {
        *min_code = min_sample;
        *max_code = max_sample;
        *avg_code = (int32_t)(sum / samples);
    }
    xSemaphoreGive(s_lock);
    return err;
}

int32_t ads1256_code_to_mv(int32_t code)
{
    ads1256_config_t cfg = ads1256_get_config();
    const float fs_mv = (2.0f * (float)cfg.vref_mv) / (float)cfg.pga;
    return (int32_t)(((float)code / 8388607.0f) * fs_mv);
}
