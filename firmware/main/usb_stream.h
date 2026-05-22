#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "scope_config.h"
#include "frame.h"

esp_err_t usb_stream_init(void);
esp_err_t usb_stream_send_bytes(const uint8_t *data, size_t len, uint32_t timeout_ms);
esp_err_t usb_stream_send_frame(const scope_frame_meta_t *meta,
                                const uint8_t *payload,
                                size_t payload_len);
esp_err_t usb_stream_send_json_line(const char *line);
size_t usb_stream_read_rx(uint8_t *out, size_t out_capacity, uint32_t timeout_ms);
bool usb_stream_is_connected(void);
