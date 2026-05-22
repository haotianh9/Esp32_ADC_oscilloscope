#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "driver/gpio.h"
#include "esp_err.h"

esp_err_t selftest_pwm_init(void);
esp_err_t selftest_pwm_configure(uint32_t freq_hz, uint8_t duty_percent, bool enabled);
esp_err_t selftest_pwm_configure_pin(gpio_num_t gpio, uint32_t freq_hz, uint8_t duty_percent, bool enabled);
void selftest_pwm_get(uint32_t *freq_hz, uint8_t *duty_percent, bool *enabled);
gpio_num_t selftest_pwm_gpio(void);
int selftest_pwm_level(void);
