#include "trigger.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static SemaphoreHandle_t s_lock;
static trigger_config_t s_cfg = {
    .armed = false,
    .edge = TRIGGER_EDGE_RISING,
    .level_mv = 1200,
    .hysteresis_mv = 20,
    .pre_trigger = 0.25f,
    .samples = 4096,
};
static bool s_rearmed;
static int32_t s_previous_mv;

void trigger_init(void)
{
    s_lock = xSemaphoreCreateMutex();
}

esp_err_t trigger_configure(const trigger_config_t *config)
{
    if (!config || config->samples == 0 || config->pre_trigger < 0.0f || config->pre_trigger >= 1.0f) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_cfg = *config;
    s_rearmed = false;
    s_previous_mv = config->level_mv;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

void trigger_stop(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_cfg.armed = false;
    s_rearmed = false;
    xSemaphoreGive(s_lock);
}

trigger_config_t trigger_get_config(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    trigger_config_t copy = s_cfg;
    xSemaphoreGive(s_lock);
    return copy;
}

trigger_event_t trigger_process_mv(int32_t sample_mv)
{
    trigger_event_t event = TRIGGER_EVENT_NONE;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_cfg.armed) {
        const int32_t low = s_cfg.level_mv - s_cfg.hysteresis_mv;
        const int32_t high = s_cfg.level_mv + s_cfg.hysteresis_mv;

        if (s_cfg.edge == TRIGGER_EDGE_RISING) {
            if (sample_mv < low) {
                s_rearmed = true;
            }
            if (s_rearmed && s_previous_mv < high && sample_mv >= high) {
                event = TRIGGER_EVENT_HIT;
                s_cfg.armed = false;
            }
        } else {
            if (sample_mv > high) {
                s_rearmed = true;
            }
            if (s_rearmed && s_previous_mv > low && sample_mv <= low) {
                event = TRIGGER_EVENT_HIT;
                s_cfg.armed = false;
            }
        }
    }
    s_previous_mv = sample_mv;
    xSemaphoreGive(s_lock);

    return event;
}
