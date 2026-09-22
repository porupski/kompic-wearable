/**
 * @file ui_animations.c
 * @brief Reusable LVGL animation helpers.
 *
 * See ui_animations.h for API contract.
 * See LVGL9_Kompic_Architecture.md §5.5 for design rationale.
 * See smartwatch demo lv_demo_smartwatch.c:280+ for the reference impl.
 */

#include "ui_animations.h"
#include "boot_display.h"     // LCD_H_RES, LCD_V_RES
#include "esp_log.h"

static const char *TAG = "UI_ANIM";

// ---------------------------------------------------------------------------
// exec cbs -- LVGL calls these each anim tick with the interpolated value
// ---------------------------------------------------------------------------

static void anim_translate_y_cb(void *var, int32_t v)
{
    lv_obj_set_style_translate_y((lv_obj_t *)var, v, 0);
}

static void anim_translate_x_cb(void *var, int32_t v)
{
    lv_obj_set_style_translate_x((lv_obj_t *)var, v, 0);
}

static void anim_opa_cb(void *var, int32_t v)
{
    lv_obj_set_style_opa((lv_obj_t *)var, (lv_opa_t)v, 0);
}

// ---------------------------------------------------------------------------
// Internal builder for the two slide variants
// ---------------------------------------------------------------------------

static void slide(lv_obj_t *obj, lv_dir_t dir, int32_t from, int32_t to,
                  uint32_t dur_ms, uint32_t delay_ms, bool ease_in)
{
    if (!obj) return;

    lv_anim_exec_xcb_t cb = NULL;
    switch (dir) {
        case LV_DIR_TOP:
        case LV_DIR_BOTTOM:
            cb = anim_translate_y_cb;
            break;
        case LV_DIR_LEFT:
        case LV_DIR_RIGHT:
            cb = anim_translate_x_cb;
            break;
        default:
            ESP_LOGW(TAG, "slide: unsupported dir=%d (silent no-op)", (int)dir);
            return;
    }

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, obj);
    lv_anim_set_values(&a, from, to);
    lv_anim_set_duration(&a, dur_ms);
    lv_anim_set_delay(&a, delay_ms);
    lv_anim_set_exec_cb(&a, cb);
    lv_anim_set_path_cb(&a,
                        ease_in ? lv_anim_path_ease_in
                                : lv_anim_path_ease_out);
    lv_anim_start(&a);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void kw_ui_animate_slide_in(lv_obj_t *obj, lv_dir_t from_dir,
                            uint32_t dur_ms, uint32_t delay_ms)
{
    int32_t from = 0;
    switch (from_dir) {
        case LV_DIR_TOP:    from = -LCD_V_RES; break;
        case LV_DIR_BOTTOM: from =  LCD_V_RES; break;
        case LV_DIR_LEFT:   from = -LCD_H_RES; break;
        case LV_DIR_RIGHT:  from =  LCD_H_RES; break;
        default:
            ESP_LOGW(TAG, "slide_in: unsupported dir=%d", (int)from_dir);
            return;
    }
    ESP_LOGD(TAG, "slide_in obj=%p from_dir=%d dur=%lu ms",
             obj, (int)from_dir, (unsigned long)dur_ms);
    slide(obj, from_dir, from, 0, dur_ms, delay_ms, /*ease_in=*/false);
}

void kw_ui_animate_slide_out(lv_obj_t *obj, lv_dir_t to_dir,
                             uint32_t dur_ms, uint32_t delay_ms)
{
    int32_t to = 0;
    switch (to_dir) {
        case LV_DIR_TOP:    to = -LCD_V_RES; break;
        case LV_DIR_BOTTOM: to =  LCD_V_RES; break;
        case LV_DIR_LEFT:   to = -LCD_H_RES; break;
        case LV_DIR_RIGHT:  to =  LCD_H_RES; break;
        default:
            ESP_LOGW(TAG, "slide_out: unsupported dir=%d", (int)to_dir);
            return;
    }
    ESP_LOGD(TAG, "slide_out obj=%p to_dir=%d dur=%lu ms",
             obj, (int)to_dir, (unsigned long)dur_ms);
    slide(obj, to_dir, 0, to, dur_ms, delay_ms, /*ease_in=*/true);
}

void kw_ui_animate_opa(lv_obj_t *obj, lv_opa_t target_opa,
                       uint32_t dur_ms, uint32_t delay_ms)
{
    if (!obj) return;
    int32_t from = (int32_t)lv_obj_get_style_opa(obj, 0);
    ESP_LOGD(TAG, "opa obj=%p from=%d to=%d dur=%lu ms",
             obj, (int)from, (int)target_opa, (unsigned long)dur_ms);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, obj);
    lv_anim_set_values(&a, from, (int32_t)target_opa);
    lv_anim_set_duration(&a, dur_ms);
    lv_anim_set_delay(&a, delay_ms);
    lv_anim_set_exec_cb(&a, anim_opa_cb);
    lv_anim_set_path_cb(&a, lv_anim_path_linear);
    lv_anim_start(&a);
}
