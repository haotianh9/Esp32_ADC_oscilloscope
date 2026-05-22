#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "scope_config.h"

#define SCOPE_FRAME_HEADER_LEN 30
#define SCOPE_FRAME_CRC_LEN 4
#define SCOPE_FRAME_MAX_PAYLOAD 2048
#define SCOPE_FRAME_MAX_LEN (SCOPE_FRAME_HEADER_LEN + SCOPE_FRAME_MAX_PAYLOAD + SCOPE_FRAME_CRC_LEN)

typedef struct {
    scope_frame_type_t type;
    scope_source_t source;
    uint16_t channelmask;
    uint32_t sample_hz;
    uint64_t t0_us;
    uint32_t dt_ns;
    scope_sample_format_t format;
    uint16_t nsamples;
} scope_frame_meta_t;

uint32_t scope_crc32(const uint8_t *data, size_t len);

esp_err_t scope_frame_build(const scope_frame_meta_t *meta,
                            const uint8_t *payload,
                            size_t payload_len,
                            uint8_t *out,
                            size_t out_capacity,
                            size_t *out_len);
