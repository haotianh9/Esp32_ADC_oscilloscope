#include "usb_stream.h"

#include <string.h>
#include "driver/uart.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "tinyusb.h"
#include "tinyusb_cdc_acm.h"
#include "tinyusb_default_config.h"
#include "sdkconfig.h"

static const char *TAG = "usb_stream";

typedef struct {
    uint8_t data[512];
    size_t len;
} stream_rx_chunk_t;

static QueueHandle_t s_rx_queue;
static SemaphoreHandle_t s_tx_lock;
static SemaphoreHandle_t s_frame_lock;
static bool s_connected;
static uint8_t s_rx_tmp[CONFIG_TINYUSB_CDC_RX_BUFSIZE];
static uint8_t s_uart_rx_tmp[512];
static uint8_t s_frame_buf[SCOPE_FRAME_MAX_LEN];
static bool s_tinyusb_ready;

static void enqueue_rx(const uint8_t *data, size_t len)
{
    if (!data || len == 0) {
        return;
    }

    stream_rx_chunk_t chunk = {
        .len = len < sizeof(chunk.data) ? len : sizeof(chunk.data),
    };
    memcpy(chunk.data, data, chunk.len);
    (void)xQueueSend(s_rx_queue, &chunk, 0);
}

static void usb_rx_callback(int itf, cdcacm_event_t *event)
{
    (void)event;

    size_t rx_size = 0;
    if (tinyusb_cdcacm_read(itf, s_rx_tmp, sizeof(s_rx_tmp), &rx_size) != ESP_OK || rx_size == 0) {
        return;
    }

    enqueue_rx(s_rx_tmp, rx_size);
}

static void usb_line_state_callback(int itf, cdcacm_event_t *event)
{
    (void)itf;
    s_connected = event->line_state_changed_data.dtr != 0;
    ESP_LOGI(TAG, "CDC line state: DTR=%d RTS=%d",
             event->line_state_changed_data.dtr,
             event->line_state_changed_data.rts);
}

static void uart_rx_task(void *arg)
{
    (void)arg;

    while (true) {
        int n = uart_read_bytes(UART_NUM_0, s_uart_rx_tmp, sizeof(s_uart_rx_tmp), pdMS_TO_TICKS(100));
        if (n > 0) {
            enqueue_rx(s_uart_rx_tmp, (size_t)n);
        }
    }
}

static esp_err_t uart_stream_init(void)
{
    uart_config_t cfg = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    ESP_RETURN_ON_ERROR(uart_param_config(UART_NUM_0, &cfg), TAG, "uart config");
    esp_err_t err = uart_driver_install(UART_NUM_0, 4096, 4096, 0, NULL, 0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    BaseType_t ok = xTaskCreate(uart_rx_task, "uart_rx", 4096, NULL, 6, NULL);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "uart rx task");
    return ESP_OK;
}

esp_err_t usb_stream_init(void)
{
    s_rx_queue = xQueueCreate(16, sizeof(stream_rx_chunk_t));
    s_tx_lock = xSemaphoreCreateMutex();
    s_frame_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_rx_queue && s_tx_lock && s_frame_lock, ESP_ERR_NO_MEM, TAG, "USB queues");

    ESP_RETURN_ON_ERROR(uart_stream_init(), TAG, "uart stream");

    const tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG();
    esp_err_t err = tinyusb_driver_install(&tusb_cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "TinyUSB unavailable, continuing on UART0: %s", esp_err_to_name(err));
        return ESP_OK;
    }

    tinyusb_config_cdcacm_t acm_cfg = {
        .cdc_port = TINYUSB_CDC_ACM_0,
        .callback_rx = usb_rx_callback,
        .callback_rx_wanted_char = NULL,
        .callback_line_state_changed = NULL,
        .callback_line_coding_changed = NULL,
    };
    err = tinyusb_cdcacm_init(&acm_cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "TinyUSB CDC init failed, continuing on UART0: %s", esp_err_to_name(err));
        return ESP_OK;
    }
    err = tinyusb_cdcacm_register_callback(TINYUSB_CDC_ACM_0,
                                           CDC_EVENT_LINE_STATE_CHANGED,
                                           usb_line_state_callback);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "TinyUSB line callback failed, continuing on UART0: %s", esp_err_to_name(err));
        return ESP_OK;
    }
    s_tinyusb_ready = true;
    ESP_LOGI(TAG, "TinyUSB CDC ready");
    return ESP_OK;
}

esp_err_t usb_stream_send_bytes(const uint8_t *data, size_t len, uint32_t timeout_ms)
{
    if (!data || !len) {
        return ESP_ERR_INVALID_ARG;
    }

    if (xSemaphoreTake(s_tx_lock, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t err = ESP_OK;

    int written = uart_write_bytes(UART_NUM_0, data, len);
    if (written < 0 || (size_t)written != len) {
        err = ESP_FAIL;
    }

    if (s_tinyusb_ready && s_connected) {
        esp_err_t usb_err = tinyusb_cdcacm_write_queue(TINYUSB_CDC_ACM_0, data, len);
        if (usb_err == ESP_OK) {
            usb_err = tinyusb_cdcacm_write_flush(TINYUSB_CDC_ACM_0, timeout_ms);
        }
        if (err == ESP_OK) {
            err = usb_err;
        }
    }

    if (err == ESP_OK) {
        (void)uart_wait_tx_done(UART_NUM_0, pdMS_TO_TICKS(timeout_ms));
    }

    xSemaphoreGive(s_tx_lock);
    return err;
}

esp_err_t usb_stream_send_frame(const scope_frame_meta_t *meta,
                                const uint8_t *payload,
                                size_t payload_len)
{
    if (xSemaphoreTake(s_frame_lock, pdMS_TO_TICKS(50)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    size_t frame_len = 0;
    esp_err_t err = scope_frame_build(meta, payload, payload_len,
                                      s_frame_buf, sizeof(s_frame_buf), &frame_len);
    if (err != ESP_OK) {
        xSemaphoreGive(s_frame_lock);
        return err;
    }
    err = usb_stream_send_bytes(s_frame_buf, frame_len, 50);
    xSemaphoreGive(s_frame_lock);
    return err;
}

esp_err_t usb_stream_send_json_line(const char *line)
{
    if (!line) {
        return ESP_ERR_INVALID_ARG;
    }

    if (xSemaphoreTake(s_tx_lock, pdMS_TO_TICKS(50)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t err = ESP_OK;
    static const uint8_t newline = '\n';
    const size_t len = strlen(line);

    int written = uart_write_bytes(UART_NUM_0, line, len);
    int newline_written = uart_write_bytes(UART_NUM_0, &newline, 1);
    if (written < 0 || newline_written < 0 || (size_t)written != len || newline_written != 1) {
        err = ESP_FAIL;
    }

    if (s_tinyusb_ready && s_connected) {
        esp_err_t usb_err = tinyusb_cdcacm_write_queue(TINYUSB_CDC_ACM_0, (const uint8_t *)line, len);
        if (usb_err == ESP_OK) {
            usb_err = tinyusb_cdcacm_write_queue(TINYUSB_CDC_ACM_0, &newline, 1);
        }
        if (usb_err == ESP_OK) {
            usb_err = tinyusb_cdcacm_write_flush(TINYUSB_CDC_ACM_0, 50);
        }
        if (err == ESP_OK) {
            err = usb_err;
        }
    }

    if (err == ESP_OK) {
        (void)uart_wait_tx_done(UART_NUM_0, pdMS_TO_TICKS(50));
    }

    xSemaphoreGive(s_tx_lock);
    return err;
}

size_t usb_stream_read_rx(uint8_t *out, size_t out_capacity, uint32_t timeout_ms)
{
    if (!out || out_capacity == 0) {
        return 0;
    }

    stream_rx_chunk_t chunk = {0};
    if (xQueueReceive(s_rx_queue, &chunk, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        return 0;
    }

    const size_t n = chunk.len < out_capacity ? chunk.len : out_capacity;
    memcpy(out, chunk.data, n);
    return n;
}

bool usb_stream_is_connected(void)
{
    return s_connected;
}
