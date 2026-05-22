#include "adc_fast.h"
#include "ads1256.h"
#include "command.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "scope_config.h"
#include "selftest_pwm.h"
#include "trigger.h"
#include "usb_stream.h"

#include <stdio.h>

static const char *TAG = "scope_app";

void app_main(void)
{
    ESP_LOGI(TAG, "ESP32-S3 dual-mode oscilloscope firmware %s", SCOPE_FW_VERSION);

    ESP_ERROR_CHECK(usb_stream_init());
    trigger_init();
    ESP_ERROR_CHECK(selftest_pwm_init());
    ESP_ERROR_CHECK(adc_fast_init());
    ESP_ERROR_CHECK(ads1256_init());
    ESP_ERROR_CHECK(command_init());

    esp_err_t pwm_err = selftest_pwm_configure(50, 50, true);
    if (pwm_err != ESP_OK) {
        ESP_LOGW(TAG, "Self-test PWM start failed: %s", esp_err_to_name(pwm_err));
        (void)usb_stream_send_json_line("{\"type\":\"error\",\"cmd\":\"set_pwm\",\"message\":\"self-test pwm start failed\"}");
    }
    char boot_line[80] = {0};
    snprintf(boot_line, sizeof(boot_line), "{\"type\":\"boot\",\"fw\":\"%s\",\"baud\":115200}", SCOPE_FW_VERSION);
    (void)usb_stream_send_json_line(boot_line);
    ESP_LOGI(TAG, "Self-test PWM enabled on GPIO18, ADC input expected on GPIO4");

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
