/**
 * @file ui_notif_overlay.c
 * @brief Notification / alarm overlay — STOP / LATER on lv_layer_top().
 *
 * LIFECYCLE: lazy build / destroy.
 *   notif_overlay_show_alarm() creates the widget tree on lv_layer_top().
 *   Button callbacks or notif_overlay_dismiss() delete s_overlay and NULL it.
 *   No widgets exist when no notification is active → zero WDT risk.
 *
 * STOP callback:
 *   broker read-before-write: firing=false, snoozed=false, snooze_until=0.
 *   alarm_nvs_clear_snooze(). Destroy overlay.
 *
 * LATER callback:
 *   Compute snooze target = (current minute-of-day + snooze_minutes) % 1440.
 *   broker read-before-write: firing=false, snoozed=true, snooze_until=target.
 *   alarm_nvs_save_snooze(target). Destroy overlay.
 *
 * THEME: notif_overlay_apply_theme() is a stub hook.  Currently fixed dark.
 *
 * Core 1 only.  No I2C.  All calls inside lvgl_port_lock().
 */

#include "ui_notif_overlay.h"
#include "ui_theme_colors.h"
#include "data_broker.h"
#include "alarm.h"
#include "boot_display.h"   /* LCD_H_RES, LCD_V_RES */
#include "lvgl.h"
#include "esp_log.h"

static const char *TAG = "NOTIF_OVL";

/* ── Module-static state ─────────────────────────────────────────────── */

static lv_obj_t *s_overlay   = NULL;   /* root container on lv_layer_top() */
static uint8_t   s_alarm_id  = 0;      /* which slot fired               */
static uint8_t   s_snooze_m  = 0;      /* snooze minutes from event      */

/* ── Forward declarations ────────────────────────────────────────────── */

static void destroy_overlay(void);
static void cb_stop(lv_event_t *e);
static void cb_later(lv_event_t *e);

/* ── Helpers ─────────────────────────────────────────────────────────── */

/**
 * @brief Get current minute-of-day from broker RTC.
 * Returns 0 on failure (safe — snooze will just target 00:00+N which
 * is still a valid minute-of-day).
 */
static uint16_t get_current_mod(void)
{
    broker_rtc_data_t rtc = {0};
    broker_rtc_read(&rtc);
    if (!rtc.valid) return 0;
    return (uint16_t)(rtc.hour * 60u + rtc.minute);
}

static void destroy_overlay(void)
{
    if (s_overlay) {
        lv_obj_del(s_overlay);
        s_overlay = NULL;
        ESP_LOGI(TAG, "Overlay destroyed");
    }
}

/* ── Button callbacks ────────────────────────────────────────────────── */

static void cb_stop(lv_event_t *e)
{
    (void)e;
    ESP_LOGI(TAG, "STOP pressed (slot %u)", (unsigned)s_alarm_id);

    /* Read-before-write — preserve all fields except firing state */
    broker_alarm_data_t ad = {0};
    broker_alarm_read(&ad);
    ad.firing       = false;
    ad.snoozed      = false;
    ad.snooze_until = 0;
    broker_alarm_write(&ad);

    alarm_nvs_clear_snooze();
    destroy_overlay();
}

static void cb_later(lv_event_t *e)
{
    (void)e;

    uint16_t cur     = get_current_mod();
    uint16_t target  = (cur + s_snooze_m) % 1440u;

    ESP_LOGI(TAG, "LATER pressed (slot %u) — snooze %u min, re-fire at %02u:%02u",
             (unsigned)s_alarm_id, (unsigned)s_snooze_m,
             (unsigned)(target / 60u), (unsigned)(target % 60u));

    broker_alarm_data_t ad = {0};
    broker_alarm_read(&ad);
    ad.firing       = false;
    ad.snoozed      = true;
    ad.snooze_until = target;
    broker_alarm_write(&ad);

    alarm_nvs_save_snooze(target);
    destroy_overlay();
}

/* ── Public API ──────────────────────────────────────────────────────── */

void notif_overlay_show_alarm(uint8_t alarm_id, uint8_t snooze_minutes)
{
    /* If already showing something, tear it down first (one-at-a-time) */
    destroy_overlay();

    s_alarm_id = alarm_id;
    s_snooze_m = snooze_minutes;

    /* ── Root container on lv_layer_top() ─────────────────────────── */
    lv_obj_t *layer = lv_layer_top();

    s_overlay = lv_obj_create(layer);
    lv_obj_set_size(s_overlay, LCD_H_RES, LCD_V_RES);
    lv_obj_set_pos(s_overlay, 0, 0);
    lv_obj_set_style_bg_color(s_overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_overlay, LV_OPA_90, 0);
    lv_obj_set_style_border_width(s_overlay, 0, 0);
    lv_obj_set_style_pad_all(s_overlay, 0, 0);
    lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_SCROLLABLE);
    /* Absorb all touch — nothing below is interactive while overlay is up */
    lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_CLICKABLE);

    /* ── Alarm icon ───────────────────────────────────────────────── */
    lv_obj_t *icon = lv_label_create(s_overlay);
    lv_label_set_text(icon, LV_SYMBOL_BELL);
    lv_obj_set_style_text_color(icon, COL_ACCENT, 0);
    lv_obj_set_style_text_font(icon, UI_FONT_TITLE, 0);
    lv_obj_align(icon, LV_ALIGN_TOP_MID, 0, 50);

    /* ── "ALARM" label ────────────────────────────────────────────── */
    lv_obj_t *lbl_title = lv_label_create(s_overlay);
    lv_label_set_text(lbl_title, "ALARM");
    lv_obj_set_style_text_color(lbl_title, lv_color_white(), 0);
    lv_obj_set_style_text_font(lbl_title, UI_FONT_LABEL, 0);
    lv_obj_align_to(lbl_title, icon, LV_ALIGN_OUT_BOTTOM_MID, 0, 8);

    /* ── Alarm time ───────────────────────────────────────────────── */
    broker_alarm_data_t ad = {0};
    broker_alarm_read(&ad);

    char time_str[8] = "--:--";
    if (alarm_id < ALARM_MAX_SLOTS &&
        ad.slots[alarm_id].hour != ALARM_SLOT_EMPTY) {
        snprintf(time_str, sizeof(time_str), "%02u:%02u",
                 (unsigned)ad.slots[alarm_id].hour,
                 (unsigned)ad.slots[alarm_id].minute);
    }

    lv_obj_t *lbl_time = lv_label_create(s_overlay);
    lv_label_set_text(lbl_time, time_str);
    lv_obj_set_style_text_color(lbl_time, lv_color_white(), 0);
    lv_obj_set_style_text_font(lbl_time, UI_FONT_TITLE, 0);
    lv_obj_align(lbl_time, LV_ALIGN_CENTER, 0, -10);

    /* ── STOP button ──────────────────────────────────────────────── */
    lv_obj_t *btn_stop = lv_btn_create(s_overlay);
    lv_obj_set_size(btn_stop, 100, 44);
    lv_obj_align(btn_stop, LV_ALIGN_BOTTOM_MID, -55, -40);
    lv_obj_set_style_bg_color(btn_stop, COL_STATUS_OFFLINE, 0);  /* red */
    lv_obj_set_style_radius(btn_stop, 8, 0);
    lv_obj_add_event_cb(btn_stop, cb_stop, LV_EVENT_SHORT_CLICKED, NULL);

    lv_obj_t *lbl_stop = lv_label_create(btn_stop);
    lv_label_set_text(lbl_stop, "STOP");
    lv_obj_set_style_text_color(lbl_stop, lv_color_white(), 0);
    lv_obj_set_style_text_font(lbl_stop, UI_FONT_LABEL, 0);
    lv_obj_center(lbl_stop);

    /* ── LATER button ─────────────────────────────────────────────── */
    lv_obj_t *btn_later = lv_btn_create(s_overlay);
    lv_obj_set_size(btn_later, 100, 44);
    lv_obj_align(btn_later, LV_ALIGN_BOTTOM_MID, 55, -40);
    lv_obj_set_style_bg_color(btn_later, COL_ACCENT, 0);         /* accent */
    lv_obj_set_style_radius(btn_later, 8, 0);
    lv_obj_add_event_cb(btn_later, cb_later, LV_EVENT_SHORT_CLICKED, NULL);

    char later_str[24];
    snprintf(later_str, sizeof(later_str), "LATER %um", (unsigned)snooze_minutes);
    lv_obj_t *lbl_later = lv_label_create(btn_later);
    lv_label_set_text(lbl_later, later_str);
    lv_obj_set_style_text_color(lbl_later, lv_color_white(), 0);
    lv_obj_set_style_text_font(lbl_later, UI_FONT_LABEL, 0);
    lv_obj_center(lbl_later);

    ESP_LOGI(TAG, "Alarm overlay shown: slot %u  %s  snooze=%um",
             (unsigned)alarm_id, time_str, (unsigned)snooze_minutes);
}

void notif_overlay_dismiss(void)
{
    if (!s_overlay) return;
    ESP_LOGI(TAG, "Overlay dismissed (auto-dismiss or external)");
    destroy_overlay();
}

bool notif_overlay_is_visible(void)
{
    return s_overlay != NULL;
}

void notif_overlay_apply_theme(ui_theme_t theme)
{
    (void)theme;
    /*
     * Stub — alarm overlay uses fixed dark styling.
     * When themed overlays are needed, rebuild colours here.
     * Different overlay types (alarm vs generic notif) can branch
     * on an internal type flag added later.
     */
}