#include "frame.h"

#include <string.h>

static uint32_t s_seq;

static void put_u16(uint8_t *buf, size_t *idx, uint16_t value)
{
    buf[(*idx)++] = (uint8_t)(value & 0xff);
    buf[(*idx)++] = (uint8_t)((value >> 8) & 0xff);
}

static void put_u32(uint8_t *buf, size_t *idx, uint32_t value)
{
    for (int i = 0; i < 4; ++i) {
        buf[(*idx)++] = (uint8_t)((value >> (8 * i)) & 0xff);
    }
}

static void put_u64(uint8_t *buf, size_t *idx, uint64_t value)
{
    for (int i = 0; i < 8; ++i) {
        buf[(*idx)++] = (uint8_t)((value >> (8 * i)) & 0xff);
    }
}

uint32_t scope_crc32(const uint8_t *data, size_t len)
{
    uint32_t crc = 0xffffffffu;

    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit) {
            uint32_t mask = -(crc & 1u);
            crc = (crc >> 1) ^ (0xedb88320u & mask);
        }
    }

    return ~crc;
}

esp_err_t scope_frame_build(const scope_frame_meta_t *meta,
                            const uint8_t *payload,
                            size_t payload_len,
                            uint8_t *out,
                            size_t out_capacity,
                            size_t *out_len)
{
    if (!meta || !out || !out_len || payload_len > SCOPE_FRAME_MAX_PAYLOAD) {
        return ESP_ERR_INVALID_ARG;
    }
    if (payload_len && !payload) {
        return ESP_ERR_INVALID_ARG;
    }
    if (out_capacity < SCOPE_FRAME_HEADER_LEN + payload_len + SCOPE_FRAME_CRC_LEN) {
        return ESP_ERR_NO_MEM;
    }

    size_t idx = 0;
    put_u16(out, &idx, SCOPE_FRAME_MAGIC);
    out[idx++] = SCOPE_FRAME_VERSION;
    out[idx++] = (uint8_t)meta->type;
    put_u32(out, &idx, s_seq++);
    out[idx++] = (uint8_t)meta->source;
    put_u16(out, &idx, meta->channelmask);
    put_u32(out, &idx, meta->sample_hz);
    put_u64(out, &idx, meta->t0_us);
    put_u32(out, &idx, meta->dt_ns);
    out[idx++] = (uint8_t)meta->format;
    put_u16(out, &idx, meta->nsamples);

    if (payload_len) {
        memcpy(&out[idx], payload, payload_len);
        idx += payload_len;
    }

    const uint32_t crc = scope_crc32(&out[2], (SCOPE_FRAME_HEADER_LEN - 2) + payload_len);
    put_u32(out, &idx, crc);
    *out_len = idx;

    return ESP_OK;
}
