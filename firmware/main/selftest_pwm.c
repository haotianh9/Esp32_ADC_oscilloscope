#include "selftest_pwm.h"

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "scope_config.h"

static const char *TAG = "selftest_pwm";

#define PWM_LEDC_MODE LEDC_LOW_SPEED_MODE
#define PWM_LEDC_TIMER LEDC_TIMER_0
#define PWM_LEDC_CHANNEL LEDC_CHANNEL_0
#define PWM_LEDC_RESOLUTION LEDC_TIMER_8_BIT
#define PWM_LEDC_MAX_DUTY ((1u << 8) - 1u)

static SemaphoreHandle_t s_lock;
static esp_timer_handle_t s_timer;
static gpio_num_t s_pwm_gpio = SCOPE_PWM_GPIO;
static uint32_t s_freq_hz = 1000;
static uint8_t s_duty_percent = 50;
static bool s_enabled;
static volatile bool s_output_high;
static volatile uint32_t s_high_us = 500;
static volatile uint32_t s_low_us = 500;
static volatile bool s_timer_enabled;

static void schedule_next_edge(uint32_t delay_us)
{
    if (!s_timer || !s_timer_enabled) {
        return;
    }
    if (delay_us < 50) {
        delay_us = 50;
    }
    (void)esp_timer_start_once(s_timer, delay_us);
}

static void pwm_timer_cb(void *arg)
{
    (void)arg;

    if (!s_timer_enabled) {
        gpio_set_level(s_pwm_gpio, 0);
        s_output_high = false;
        return;
    }

    s_output_high = !s_output_high;
    gpio_set_level(s_pwm_gpio, s_output_high ? 1 : 0);
    schedule_next_edge(s_output_high ? s_high_us : s_low_us);
}

static esp_err_t configure_pwm_gpio(gpio_num_t gpio)
{
    if (!GPIO_IS_VALID_OUTPUT_GPIO(gpio)) {
        return ESP_ERR_INVALID_ARG;
    }

    gpio_hold_dis(gpio);
    gpio_reset_pin(gpio);
    gpio_config_t gpio_cfg = {
        .pin_bit_mask = 1ull << gpio,
        .mode = GPIO_MODE_INPUT_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&gpio_cfg), TAG, "gpio config");
    return gpio_set_drive_capability(gpio, GPIO_DRIVE_CAP_3);
}

static esp_err_t apply_static_level(gpio_num_t gpio, int level)
{
    if (s_timer) {
        (void)esp_timer_stop(s_timer);
    }
    s_timer_enabled = false;
    ESP_RETURN_ON_ERROR(configure_pwm_gpio(gpio), TAG, "static gpio");
    gpio_set_level(gpio, level ? 1 : 0);
    s_output_high = level ? true : false;
    return ESP_OK;
}

static esp_err_t apply_timer_pwm(gpio_num_t gpio, uint8_t duty_percent)
{
    (void)duty_percent;
    ESP_RETURN_ON_ERROR(configure_pwm_gpio(gpio), TAG, "timer gpio");
    s_timer_enabled = true;
    s_output_high = true;
    gpio_set_level(gpio, 1);
    schedule_next_edge(s_high_us);
    return ESP_OK;
}

static esp_err_t apply_ledc_pwm(gpio_num_t gpio, uint32_t freq_hz, uint8_t duty_percent)
{
    if (s_timer) {
        (void)esp_timer_stop(s_timer);
    }
    s_timer_enabled = false;

    gpio_reset_pin(gpio);
    ESP_RETURN_ON_ERROR(gpio_set_drive_capability(gpio, GPIO_DRIVE_CAP_3), TAG, "ledc drive");

    ledc_timer_config_t timer_cfg = {
        .speed_mode = PWM_LEDC_MODE,
        .duty_resolution = PWM_LEDC_RESOLUTION,
        .timer_num = PWM_LEDC_TIMER,
        .freq_hz = freq_hz,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer_cfg), TAG, "ledc timer");

    const uint32_t duty = (PWM_LEDC_MAX_DUTY * duty_percent) / 100u;
    ledc_channel_config_t channel_cfg = {
        .gpio_num = gpio,
        .speed_mode = PWM_LEDC_MODE,
        .channel = PWM_LEDC_CHANNEL,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = PWM_LEDC_TIMER,
        .duty = duty,
        .hpoint = 0,
        .flags.output_invert = 0,
    };
    ESP_RETURN_ON_ERROR(ledc_channel_config(&channel_cfg), TAG, "ledc channel");
    ESP_RETURN_ON_ERROR(ledc_set_duty(PWM_LEDC_MODE, PWM_LEDC_CHANNEL, duty), TAG, "ledc duty");
    return ledc_update_duty(PWM_LEDC_MODE, PWM_LEDC_CHANNEL);
}

esp_err_t selftest_pwm_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_lock, ESP_ERR_NO_MEM, TAG, "lock");

    ESP_RETURN_ON_ERROR(apply_static_level(s_pwm_gpio, 0), TAG, "gpio");
    const esp_timer_create_args_t timer_args = {
        .callback = pwm_timer_cb,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "selftest_pwm",
    };
    ESP_RETURN_ON_ERROR(esp_timer_create(&timer_args, &s_timer), TAG, "timer");
    return ESP_OK;
}

esp_err_t selftest_pwm_configure(uint32_t freq_hz, uint8_t duty_percent, bool enabled)
{
    return selftest_pwm_configure_pin(s_pwm_gpio, freq_hz, duty_percent, enabled);
}

esp_err_t selftest_pwm_configure_pin(gpio_num_t gpio, uint32_t freq_hz, uint8_t duty_percent, bool enabled)
{
    if (freq_hz == 0 || duty_percent > 100) {
        return ESP_ERR_INVALID_ARG;
    }

    uint32_t period_us = 1000000u / freq_hz;
    if (period_us == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    uint32_t high_us = (period_us * duty_percent) / 100u;
    uint32_t low_us = period_us - high_us;

    if (s_timer) {
        (void)esp_timer_stop(s_timer);
    }
    s_timer_enabled = false;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (gpio != s_pwm_gpio) {
        gpio_set_level(s_pwm_gpio, 0);
        s_pwm_gpio = gpio;
    }
    s_freq_hz = freq_hz;
    s_duty_percent = duty_percent;
    s_enabled = enabled;
    s_high_us = high_us;
    s_low_us = low_us;
    xSemaphoreGive(s_lock);

    if (!enabled || duty_percent == 0) {
        return apply_static_level(gpio, 0);
    }
    if (duty_percent == 100) {
        return apply_static_level(gpio, 1);
    }

    if (freq_hz < 1000) {
        return apply_timer_pwm(gpio, duty_percent);
    }

    esp_err_t err = apply_ledc_pwm(gpio, freq_hz, duty_percent);
    if (err == ESP_OK) {
        return ESP_OK;
    }
    ESP_LOGW(TAG, "LEDC PWM failed (%s), falling back to timer PWM", esp_err_to_name(err));
    return apply_timer_pwm(gpio, duty_percent);
}

void selftest_pwm_get(uint32_t *freq_hz, uint8_t *duty_percent, bool *enabled)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (freq_hz) {
        *freq_hz = s_freq_hz;
    }
    if (duty_percent) {
        *duty_percent = s_duty_percent;
    }
    if (enabled) {
        *enabled = s_enabled;
    }
    xSemaphoreGive(s_lock);
}

gpio_num_t selftest_pwm_gpio(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    gpio_num_t gpio = s_pwm_gpio;
    xSemaphoreGive(s_lock);
    return gpio;
}

int selftest_pwm_level(void)
{
    return gpio_get_level(selftest_pwm_gpio());
}
