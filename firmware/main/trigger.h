#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef enum {
    TRIGGER_EDGE_RISING = 0,
    TRIGGER_EDGE_FALLING = 1,
} trigger_edge_t;

typedef struct {
    bool armed;
    trigger_edge_t edge;
    int32_t level_mv;
    int32_t hysteresis_mv;
    float pre_trigger;
    uint32_t samples;
} trigger_config_t;

typedef enum {
    TRIGGER_EVENT_NONE = 0,
    TRIGGER_EVENT_HIT = 1,
} trigger_event_t;

void trigger_init(void);
esp_err_t trigger_configure(const trigger_config_t *config);
void trigger_stop(void);
trigger_config_t trigger_get_config(void);
trigger_event_t trigger_process_mv(int32_t sample_mv);
