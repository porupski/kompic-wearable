/**
 * @file ui_lock_screen.c
 * @brief Display sleep state machine — backlight timeout + touch block.
 *
 * No lock screen overlay. No "Locked" text. No unlock gesture required.
 *
 * STATE MODEL:
 *   AWAKE  → full backlight, touch enabled (absorber hidden)
 *   ASLEEP → backlight 0, invisible touch absorber blocks pocket taps
 *
 * SLEEP TRIGGER:
 *   - Auto: DISPLAY_SLEEP_MS ms of no interaction
 *   - Manual: power button short press while awake → g_display_sleep = true
 *
 * WAKE TRIGGER:
 *   - Power button short press while asleep → g_wake_display = true
 *   - IMU raise-to-wake (future): Core 0 sets g_wake_display = true — free slot-in
 *
 * Core 1 only. Called from task_ui_refresh_fn() inside lvgl_port_lock().
 */

#include "ui_lock_screen.h"
#include "power_flags.h"       // g_wake_display, g_display_sleep
#include "boot_display.h"      // backlight_set_brightness(), LCD_H_RES, LCD_V_RES
#include "data_broker.h"       // g_saved_brightness
#include "esp_timer.h"
#include "esp_log.h"
#include "lvgl.h"

static const char *TAG = "DISP_SLEEP";

#define DISPLAY_SLEEP_MS  10000   // 10s idle → sleep

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

static bool      s_asleep       = false;
static int64_t   s_last_wake_us = 0;
static lv_obj_t *s_absorber     = NULL;  // invisible touch blocker while asleep

// ---------------------------------------------------------------------------
// Internal
// ---------------------------------------------------------------------------

static void do_sleep(void)
{
    if (s_asleep) return;
    s_asleep = true;
    backlight_set_brightness(0);
    if (s_absorber) lv_obj_clear_flag(s_absorber, LV_OBJ_FLAG_HIDDEN);
    ESP_LOGI(TAG, "Display sleep");
}

static void do_wake(void)
{
    if (!s_asleep) return;
    s_asleep = false;
    s_last_wake_us = esp_timer_get_time();
    backlight_set_brightness(g_saved_brightness);
    if (s_absorber) lv_obj_add_flag(s_absorber, LV_OBJ_FLAG_HIDDEN);
    ESP_LOGI(TAG, "Display wake");
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void lock_screen_init(void)
{
    // Invisible full-screen touch absorber — shown only while asleep
    s_absorber = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_absorber, LCD_H_RES, LCD_V_RES);
    lv_obj_set_pos(s_absorber, 0, 0);
    lv_obj_set_style_bg_opa(s_absorber, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_absorber, 0, 0);
    lv_obj_set_style_pad_all(s_absorber, 0, 0);
    lv_obj_add_flag(s_absorber, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(s_absorber, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_absorber, LV_OBJ_FLAG_HIDDEN);  // hidden while awake

    s_last_wake_us = esp_timer_get_time();
    ESP_LOGI(TAG, "Display sleep init OK");
}

void lock_screen_poll(void)
{
    // -- Wake request (power button or IMU raise-to-wake) ---------------------
    if (g_wake_display) {
        g_wake_display = false;
        do_wake();
        return;
    }

    // -- Manual sleep request (power button while awake) ----------------------
    if (g_display_sleep) {
        g_display_sleep = false;
        do_sleep();
        return;
    }

    // -- Auto-sleep idle timer ------------------------------------------------
    // if (!s_asleep) {
    //     int64_t elapsed_ms = (esp_timer_get_time() - s_last_wake_us) / 1000LL;
    //     if (elapsed_ms >= DISPLAY_SLEEP_MS) {
    //         do_sleep();
    //     }
    // }
}

void display_sleep_reset_timer(void)
{
    s_last_wake_us = esp_timer_get_time();
}

bool display_is_asleep(void)
{
    return s_asleep;
}