/**
 * @file veml6030_cmd.c
 * @brief VEML6030 + backlight command surface -- see veml6030_cmd.h.
 */

#include "veml6030_cmd.h"
#include "veml6030.h"
#include "data_broker.h"
#include "ui_broker.h"          // g_auto_brightness + save_async
#include "boot_display.h"       // backlight_set_brightness()

#include "driver/i2c.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"

#include <stdio.h>
#include <string.h>

extern SemaphoreHandle_t g_i2c_mutex;   // I2C bus 0 -- VEML6030 lives there.

static const char *TAG = "VEML_CMD";

#define VEML_I2C_PORT       I2C_NUM_0
#define VEML_MUTEX_TIMEOUT  pdMS_TO_TICKS(200)

// Snapshot the four UI globals into a settings struct and enqueue an async
// NVS save. Called after any cmd-surface mutation of the persisted state.
static void save_ui_settings(void)
{
    ui_settings_t cfg = {
        .theme           = g_ui_theme,
        .brightness      = g_saved_brightness,
        .blue_light_on   = g_blue_light_on,
        .auto_brightness = g_auto_brightness,
    };
    ui_settings_save_async(&cfg);
}

// ── enable ──────────────────────────────────────────────────────────────────

esp_err_t veml6030_cmd_enable_set(bool on)
{
    broker_light_set_enabled(on);
    ESP_LOGI(TAG, "sensor %s", on ? "ENABLED" : "DISABLED");
    return ESP_OK;
}

bool veml6030_cmd_enable_get(void) { return broker_light_get_enabled(); }

// ── auto-brightness ─────────────────────────────────────────────────────────

esp_err_t veml6030_cmd_auto_brightness_set(bool on)
{
    g_auto_brightness = on;
    save_ui_settings();
    ESP_LOGI(TAG, "auto-brightness %s", on ? "ENABLED" : "DISABLED");
    return ESP_OK;
}

bool veml6030_cmd_auto_brightness_get(void) { return g_auto_brightness; }

// ── manual brightness ───────────────────────────────────────────────────────

esp_err_t veml6030_cmd_brightness_set(uint8_t pct)
{
    if (pct > 100) pct = 100;
    if (pct < 1)   pct = 1;
    g_saved_brightness = pct;
    if (!g_auto_brightness) {
        backlight_set_brightness(pct);
    }
    save_ui_settings();
    return ESP_OK;
}

uint8_t veml6030_cmd_brightness_get(void) { return g_saved_brightness; }

// ── blue-light filter ───────────────────────────────────────────────────────

esp_err_t veml6030_cmd_blue_light_set(bool on)
{
    g_blue_light_on = on;
    save_ui_settings();
    // Visual overlay flips on next light_tile_update() -- the sync guard in
    // the tile picks up the changed global. Keeps the cmd surface free of
    // LVGL calls (would need lvgl_port_lock() from arbitrary cores).
    return ESP_OK;
}

bool veml6030_cmd_blue_light_get(void) { return g_blue_light_on; }

// ── dump ────────────────────────────────────────────────────────────────────

static const struct { uint8_t reg; const char *name; } k_dump_regs[] = {
    { VEML6030_REG_ALS_CONF,     "ALS_CONF    " },
    { VEML6030_REG_ALS_WH,       "ALS_WH      " },
    { VEML6030_REG_ALS_WL,       "ALS_WL      " },
    { VEML6030_REG_POWER_SAVING, "POWER_SAVING" },
    { VEML6030_REG_ALS,          "ALS_DATA    " },
    { VEML6030_REG_WHITE,        "WHITE       " },
    { VEML6030_REG_ALS_INT,      "ALS_INT     " },
};

// Local 16-bit reg reader (VEML registers are 16-bit, LSB-first). Uses the
// same legacy driver/i2c.h helper the veml6030 driver uses internally.
static esp_err_t dump_read_reg16(uint8_t reg, uint16_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    uint8_t buf[2] = {0};
    esp_err_t r = i2c_master_write_read_device(VEML_I2C_PORT, VEML6030_ADDR,
                                               &reg, 1, buf, 2,
                                               pdMS_TO_TICKS(20));
    if (r == ESP_OK) *out = (uint16_t)buf[0] | ((uint16_t)buf[1] << 8);
    return r;
}

void veml6030_cmd_dump(void)
{
    printf("[LIGHT] %s @ 0x%02X (I2C bus 0)\n",
           veml6030_get_chip_name(), VEML6030_ADDR);

    if (xSemaphoreTake(g_i2c_mutex, VEML_MUTEX_TIMEOUT) != pdTRUE) {
        printf("[LIGHT] mutex timeout -- bus busy\n");
        return;
    }
    for (size_t i = 0; i < sizeof(k_dump_regs) / sizeof(k_dump_regs[0]); i++) {
        uint16_t v = 0xFFFF;
        esp_err_t r = dump_read_reg16(k_dump_regs[i].reg, &v);
        if (r == ESP_OK) {
            printf("[LIGHT]   0x%02X %s = 0x%04X\n",
                   k_dump_regs[i].reg, k_dump_regs[i].name, v);
        } else {
            printf("[LIGHT]   0x%02X %s = READ_FAIL (%s)\n",
                   k_dump_regs[i].reg, k_dump_regs[i].name, esp_err_to_name(r));
        }
    }
    xSemaphoreGive(g_i2c_mutex);

    veml6030_gain_t g = 0; veml6030_it_t it = 0;
    veml6030_get_range(&g, &it);
    printf("[LIGHT] gain=%d  it_idx=%d  lx_per_count=%.4f\n",
           (int)g, (int)it, (double)veml6030_current_resolution());

    broker_light_data_t ld = {0};
    broker_light_read(&ld);
    printf("[LIGHT] broker: en=%d lux=%.1f auto_br=%u%%  age=%u ms\n",
           broker_light_get_enabled() ? 1 : 0,
           (double)ld.lux, (unsigned)ld.auto_brightness,
           (unsigned)ld.last_update_ms);
    printf("[LIGHT] backlight: manual=%u%%  auto=%d  blue_filter=%d\n",
           (unsigned)g_saved_brightness,
           g_auto_brightness ? 1 : 0,
           g_blue_light_on   ? 1 : 0);
}

// ── status summary ──────────────────────────────────────────────────────────

void veml6030_cmd_status_summary(char *out, size_t max)
{
    if (!out || max == 0) return;
    broker_light_data_t ld = {0};
    broker_light_read(&ld);
    snprintf(out, max,
             "en=%d lux=%.1f br=%u%% auto=%d blue=%d",
             broker_light_get_enabled() ? 1 : 0,
             (double)ld.lux,
             (unsigned)g_saved_brightness,
             g_auto_brightness ? 1 : 0,
             g_blue_light_on   ? 1 : 0);
}
