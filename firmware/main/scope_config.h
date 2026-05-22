#pragma once

#include <stdint.h>
#include "driver/gpio.h"

#define SCOPE_FW_VERSION "0.1.5"

#define SCOPE_PWM_GPIO GPIO_NUM_18
#define SCOPE_ADC_GPIO GPIO_NUM_4

#define SCOPE_ADS1256_SCLK GPIO_NUM_12
#define SCOPE_ADS1256_MOSI GPIO_NUM_11
#define SCOPE_ADS1256_MISO GPIO_NUM_13
#define SCOPE_ADS1256_CS GPIO_NUM_14
#define SCOPE_ADS1256_DRDY GPIO_NUM_21
#define SCOPE_ADS1256_RESET GPIO_NUM_15
#define SCOPE_ADS1256_SYNC_PDWN GPIO_NUM_16

#define SCOPE_FRAME_VERSION 1
#define SCOPE_FRAME_MAGIC 0xA55A

typedef enum {
    SCOPE_SOURCE_ESP_ADC = 0,
    SCOPE_SOURCE_ADS1256 = 1,
} scope_source_t;

typedef enum {
    SCOPE_FRAME_DATA = 1,
    SCOPE_FRAME_STATUS = 2,
    SCOPE_FRAME_ACK = 3,
    SCOPE_FRAME_ERROR = 4,
} scope_frame_type_t;

typedef enum {
    SCOPE_FORMAT_U16 = 1,
    SCOPE_FORMAT_S24_IN_I32 = 2,
    SCOPE_FORMAT_F32 = 3,
} scope_sample_format_t;
