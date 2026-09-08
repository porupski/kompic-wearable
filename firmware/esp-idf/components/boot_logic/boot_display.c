/**
 * @file boot_display.c
 * @brief CO5300 QSPI panel bring-up with touch-based presence probe.
 *
 * Stage 21 §4.1a: brings the panel up on Mk1b (where CST9217 ACKs + signature
 * matches) and skips cleanly on iv7.1 (no touch/panel populated). LVGL port +
 * tile registry wiring lands in §4.1b.
 */

#include "boot_display.h"

#include "boot_hw_init.h"    // g_i2c_mutex
#include "co5300.h"
#include "cst9217.h"

#include "driver/i2c.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "BOOT_DISP";

static co5300_handle_t s_disp          = NULL;
static bool            s_present       = false;
static bool            s_touch_present = false;

// ── Presence probe ────────────────────────────────────────────────────────────
// Panel + touch share the display FPC. If CST9217 answers at 0x5A on bus 0
// with the documented ACK byte 0xAB at register 0xD000, the whole display
// module is populated. On iv7.1 (no module) or a bad Mk1b build the probe
// fails and boot_display_init() returns ESP_ERR_NOT_FOUND without touching
// the QSPI pins.
static bool touch_module_present(void)
{
    if (xSemaphoreTake(g_i2c_mutex, pdMS_TO_TICKS(50)) != pdTRUE) {
        ESP_LOGW(TAG, "probe: could not take I2C0 mutex");
        return false;
    }
    uint8_t ack = 0;
    esp_err_t err = cst9217_probe_ack(I2C_NUM_0, &ack);
    xSemaphoreGive(g_i2c_mutex);

    if (err != ESP_OK) {
        ESP_LOGI(TAG, "no CST9217 on bus 0 (i2c err=%s)", esp_err_to_name(err));
        return false;
    }
    if (ack != CST9217_ACK_VALUE) {
        ESP_LOGW(TAG, "CST9217 signature mismatch: 0x%02X (want 0x%02X)",
                 ack, CST9217_ACK_VALUE);
        return false;
    }
    ESP_LOGI(TAG, "CST9217 detected (0x%02X @ 0xD000) -- display module present",
             ack);
    return true;
}

// ── Public API ────────────────────────────────────────────────────────────────

esp_err_t boot_display_init(void)
{
    ESP_LOGI(TAG, "boot_logic v%s", BOOT_LOGIC_DRIVER_VERSION);
    if (s_present) {
        ESP_LOGW(TAG, "already initialised");
        return ESP_OK;
    }

    if (!touch_module_present()) {
        ESP_LOGI(TAG, "no CO5300 module -- skipping display bring-up");
        return ESP_ERR_NOT_FOUND;
    }

    co5300_config_t cfg = CO5300_CONFIG_DEFAULT();
    esp_err_t err = co5300_init(&cfg, &s_disp);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "co5300_init failed: %s", esp_err_to_name(err));
        s_disp = NULL;
        return err;
    }

    // co5300_init already sets brightness ~80% via WRDISBV as part of the
    // panel init sequence; leave it there for now. lvgl_ui / settings-screen
    // can call backlight_set_brightness() to adjust.

    s_present = true;
    ESP_LOGI(TAG, "CO5300 ready (%dx%d logical, brightness ~80%%)",
             LCD_H_RES, LCD_V_RES);

    // Touch chip lives on the same FPC. If it faults, log + continue -- the
    // panel is still useful with encoder-only nav and Mk1b Day-1 GPS view.
    esp_err_t terr = cst9217_init(I2C_NUM_0);
    if (terr == ESP_OK) {
        s_touch_present = true;
        ESP_LOGI(TAG, "CST9217 touch ready (task_touch_fn will run on Core 0)");
    } else {
        s_touch_present = false;
        ESP_LOGW(TAG, "CST9217 init failed: %s -- panel OK, touch disabled",
                 esp_err_to_name(terr));
    }

    return ESP_OK;
}

bool boot_display_is_present(void)
{
    return s_present;
}

void backlight_set_brightness(uint8_t pct)
{
    if (!s_present || s_disp == NULL) return;
    (void)co5300_set_brightness(s_disp, pct);
}

co5300_handle_t boot_display_get_co5300(void)
{
    return s_present ? s_disp : NULL;
}

bool boot_display_touch_is_present(void)
{
    return s_touch_present;
}
