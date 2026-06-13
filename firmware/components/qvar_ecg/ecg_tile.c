/**
 * @file ecg_tile.c
 * @brief ECG tile placeholder. Real LVGL waveform rendering lands once
 *        Qvar registers are hardware-verified (Phase 20 datasheet pass).
 *
 * The tile is registered in lvgl_ui/tile_registry.c so the tileview has a
 * working ECG slot. The init function builds a single label that reads
 * "ECG · Stub" so the user can navigate to the column and see something.
 * No broker reads, no waveform drawing.
 *
 * Architecture note: tiles live on Core 1 / LVGL. This file MUST NOT
 * include qvar_ecg.h directly -- the chip layer is Core 0 only. When
 * the waveform rendering arrives, samples will come via the data broker
 * (Tier 1: broker_ecg_data_t -- not yet defined) just like every other
 * sensor in the project.
 */

#include "ecg_tile.h"
#include "lvgl.h"
#include "esp_log.h"

static const char *TAG = "ECG_TILE";

static lv_obj_t *s_label = NULL;

// ---------------------------------------------------------------------------
// tile_desc_t contract
// ---------------------------------------------------------------------------

static void ecg_tile_build(lv_obj_t *parent)
{
    s_label = lv_label_create(parent);
    lv_label_set_text(s_label, "ECG  ·  Stub");
    lv_obj_align(s_label, LV_ALIGN_CENTER, 0, 0);
    ESP_LOGI(TAG, "ECG tile built (stub label, waveform deferred to Phase 20)");
}

static void ecg_tile_refresh(void)
{
    // No-op until the broker exposes ECG samples.
}

static void ecg_tile_apply_theme(ui_theme_t theme)
{
    if (!s_label) return;
    lv_obj_set_style_text_color(s_label,
                                (theme == UI_THEME_LIGHT) ? COL_TEXT_LIGHT : COL_TEXT_DARK,
                                0);
}

const tile_desc_t ecg_tile_desc = {
    .init           = ecg_tile_build,
    .update         = ecg_tile_refresh,
    .apply_theme    = ecg_tile_apply_theme,
    .has_subtile    = false,
    .subtile_init   = NULL,
    .subtile_update = NULL,
    .main_dirs      = LV_DIR_LEFT | LV_DIR_RIGHT,
};

esp_err_t ecg_tile_init(void)
{
    ESP_LOGI(TAG, "ecg_tile_init (legacy entry; descriptor is the real contract)");
    return ESP_OK;
}
