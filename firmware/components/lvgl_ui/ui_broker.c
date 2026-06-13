/**
 * @file ui_broker.c
 * @brief Core 1 UI settings state + async NVS save queue implementation.
 */

#include "ui_broker.h"
#include "app_nvs.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

static const char *TAG = "UI_BROKER";

volatile int8_t g_tz_offset_hours = 1;   // UTC+1 default; set from NVS at boot

volatile bool g_auto_brightness = false;

QueueHandle_t ui_settings_save_q = NULL;

QueueHandle_t ui_broker_init(void)
{
    // Depth-1 queue with overwrite semantics — only the latest settings matter.
    ui_settings_save_q = xQueueCreate(1, sizeof(ui_settings_t));
    configASSERT(ui_settings_save_q);
    ESP_LOGI(TAG, "UI settings queue created");
    return ui_settings_save_q;
}

void ui_settings_save_async(const ui_settings_t *s)
{
    if (!ui_settings_save_q) return;
    xQueueOverwrite(ui_settings_save_q, s);
}

// task_settings_saver_fn: drains the queue and writes to NVS.
// Runs unpinned at priority 2 — deliberately lowest, NVS writes are slow.
void task_settings_saver_fn(void *arg)
{
    QueueHandle_t q = (QueueHandle_t)arg;
    ui_settings_t pending = {0};

    while (1) {
        // Block indefinitely until a save is requested
        if (xQueueReceive(q, &pending, portMAX_DELAY) == pdTRUE) {
            esp_err_t ret = app_nvs_save_ui_settings(&pending);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "Settings save failed: %s", esp_err_to_name(ret));
            }
        }
    }
}
