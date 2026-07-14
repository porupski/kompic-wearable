/**
 * @file alarm.c
 * @brief Alarm module — Core 0 only.
 *
 * Stage A: NVS load/save, pattern table, broker timestamp refresh.
 * Stage C: Time-match, haptic burst/pause with escalation, snooze, auto-dismiss.
 *          Sends UI events via g_ui_event_q (Stage D wires the overlay handler).
 *
 * NVS namespace "alarm_cfg" is owned exclusively by this file.
 * Same isolation pattern as haptic.c owning "haptic_cfg".
 *
 * Architecture: Blueprint 16, Blueprint 4 §3 (read-before-write)
 */

#include "alarm.h"
#include "data_broker.h"
#include "ui_event.h"
#include "haptic.h"
#include "ws2812.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "ALARM";

// ═══ CHANGE 1: Add after the existing includes block ══════════════════════
// (after #include <stdio.h>, before static const char *TAG = "ALARM";)

extern volatile int8_t g_tz_offset_hours;

// ---------------------------------------------------------------------------
// Module identity
// ---------------------------------------------------------------------------

const char *alarm_get_module_name(void) { return "Alarm"; }
const char *alarm_get_module_desc(void) { return "Vibration alarm"; }

// ---------------------------------------------------------------------------
// Haptic alarm pattern table (Blueprint 16 §8)
// ---------------------------------------------------------------------------

static const alarm_haptic_step_t k_strong_buzz_steps[] = {
    { .effect_id = 1,  .duration_ms = 300 },
    { .effect_id = 1,  .duration_ms = 300 },
    { .effect_id = 1,  .duration_ms = 300 },
    { .effect_id = 1,  .duration_ms = 300 },
    { .effect_id = 1,  .duration_ms = 300 },
};

static const alarm_haptic_step_t k_alert_pulse_steps[] = {
    { .effect_id = 10, .duration_ms = 200 },
    { .effect_id = 0,  .duration_ms = 400 },
    { .effect_id = 10, .duration_ms = 200 },
    { .effect_id = 0,  .duration_ms = 400 },
    { .effect_id = 10, .duration_ms = 200 },
};

static const alarm_haptic_step_t k_gentle_ramp_steps[] = {
    { .effect_id = 47, .duration_ms = 500 },
    { .effect_id = 14, .duration_ms = 500 },
    { .effect_id = 1,  .duration_ms = 500 },
    { .effect_id = 1,  .duration_ms = 500 },
};

static const alarm_haptic_pattern_t k_patterns[] = {
    {
        .name       = "Strong Buzz",
        .steps      = k_strong_buzz_steps,
        .step_count = 5,
        .pause_ms   = 2000,
        .burst_ms   = 1500,
    },
    {
        .name       = "Alert Pulse",
        .steps      = k_alert_pulse_steps,
        .step_count = 5,
        .pause_ms   = 2000,
        .burst_ms   = 1400,
    },
    {
        .name       = "Gentle Ramp",
        .steps      = k_gentle_ramp_steps,
        .step_count = 4,
        .pause_ms   = 2000,
        .burst_ms   = 2000,
    },
};

const alarm_haptic_pattern_t *alarm_get_patterns(void) { return k_patterns; }
uint8_t alarm_get_pattern_count(void) { return ALARM_PATTERN_COUNT; }

// ---------------------------------------------------------------------------
// NVS helpers
// ---------------------------------------------------------------------------

#define NVS_NAMESPACE "alarm_cfg"

static void make_key(char *buf, size_t sz, uint8_t idx, const char *field)
{
    snprintf(buf, sz, "a%u_%s", (unsigned)idx, field);
}

void alarm_nvs_load_all(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "NVS namespace not found -- using defaults (all empty)");
        return;
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "NVS open failed: %s", esp_err_to_name(err));
        return;
    }

    broker_alarm_data_t ad = {0};
    broker_alarm_read(&ad);

    char key[16];
    for (uint8_t i = 0; i < ALARM_MAX_SLOTS; i++) {
        uint8_t v8 = ALARM_SLOT_EMPTY;

        make_key(key, sizeof(key), i, "hour");
        if (nvs_get_u8(h, key, &v8) == ESP_OK) ad.slots[i].hour = v8;
        else                                    ad.slots[i].hour = ALARM_SLOT_EMPTY;

        make_key(key, sizeof(key), i, "min");
        v8 = 0;
        if (nvs_get_u8(h, key, &v8) == ESP_OK) ad.slots[i].minute = v8;

        make_key(key, sizeof(key), i, "pat");
        v8 = 0;
        if (nvs_get_u8(h, key, &v8) == ESP_OK) ad.slots[i].pattern_id = v8;

        make_key(key, sizeof(key), i, "armed");
        v8 = 0;
        if (nvs_get_u8(h, key, &v8) == ESP_OK) ad.slots[i].armed = (v8 != 0);

        // NVS key kept as "buzz" so v5-persisted slots load as led_strobe.
        // The semantics changed (GPIO42 is now the WS2812 ALERT LED, not a
        // piezo) but the persisted bit carries forward without migration.
        make_key(key, sizeof(key), i, "buzz");
        v8 = 0;
        if (nvs_get_u8(h, key, &v8) == ESP_OK) ad.slots[i].led_strobe = (v8 != 0);

        if (ad.slots[i].hour != ALARM_SLOT_EMPTY) {
            ESP_LOGI(TAG, "Slot %u loaded: %02u:%02u pat=%u armed=%d strobe=%d",
                     (unsigned)i, (unsigned)ad.slots[i].hour,
                     (unsigned)ad.slots[i].minute,
                     (unsigned)ad.slots[i].pattern_id,
                     (int)ad.slots[i].armed, (int)ad.slots[i].led_strobe);
        }
    }

    // Snooze recovery
    uint16_t snz = 0;
    if (nvs_get_u16(h, "snz_until", &snz) == ESP_OK && snz > 0 && snz < 1440) {
        ad.snoozed      = true;
        ad.snooze_until = snz;
        ESP_LOGI(TAG, "Snooze recovered from NVS: re-fire at minute %u", (unsigned)snz);
    }

    nvs_close(h);
    broker_alarm_write(&ad);
}

void alarm_nvs_save_slot(uint8_t idx, const alarm_slot_t *slot)
{
    if (idx >= ALARM_MAX_SLOTS || !slot) return;

    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGE(TAG, "NVS open failed for save_slot");
        return;
    }

    char key[16];
    make_key(key, sizeof(key), idx, "hour");    nvs_set_u8(h, key, slot->hour);
    make_key(key, sizeof(key), idx, "min");     nvs_set_u8(h, key, slot->minute);
    make_key(key, sizeof(key), idx, "pat");     nvs_set_u8(h, key, slot->pattern_id);
    make_key(key, sizeof(key), idx, "armed");   nvs_set_u8(h, key, slot->armed ? 1 : 0);
    // NVS key "buzz" preserved; semantics is now WS2812 ALERT strobe.
    make_key(key, sizeof(key), idx, "buzz");    nvs_set_u8(h, key, slot->led_strobe ? 1 : 0);
    nvs_commit(h);
    nvs_close(h);

    ESP_LOGI(TAG, "Slot %u saved: %02u:%02u pat=%u armed=%d",
             (unsigned)idx, (unsigned)slot->hour, (unsigned)slot->minute,
             (unsigned)slot->pattern_id, (int)slot->armed);
}

void alarm_nvs_delete_slot(uint8_t idx)
{
    if (idx >= ALARM_MAX_SLOTS) return;
    alarm_slot_t empty = {
        .hour       = ALARM_SLOT_EMPTY,
        .minute     = 0,
        .pattern_id = 0,
        .armed      = false,
        .led_strobe = false,
    };
    alarm_nvs_save_slot(idx, &empty);
    ESP_LOGI(TAG, "Slot %u deleted", (unsigned)idx);
}

void alarm_nvs_save_snooze(uint16_t minute_of_day)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u16(h, "snz_until", minute_of_day);
    nvs_commit(h);
    nvs_close(h);
}

void alarm_nvs_clear_snooze(void)
{
    alarm_nvs_save_snooze(0);
}

// ---------------------------------------------------------------------------
// Haptic alarm sequence helpers (Stage C)
// ---------------------------------------------------------------------------

/**
 * @brief Compute 7-bit RTP amplitude from elapsed seconds.
 *        Ramps linearly from ALARM_INTENSITY_MIN_PCT to MAX over ESCALATION_SEC.
 */
static uint8_t compute_intensity_7bit(uint32_t elapsed_sec)
{
    uint32_t pct = ALARM_INTENSITY_MIN_PCT;
    if (elapsed_sec < ALARM_ESCALATION_SEC) {
        pct = ALARM_INTENSITY_MIN_PCT +
              (elapsed_sec * (ALARM_INTENSITY_MAX_PCT - ALARM_INTENSITY_MIN_PCT))
              / ALARM_ESCALATION_SEC;
    } else {
        pct = ALARM_INTENSITY_MAX_PCT;
    }
    return (uint8_t)((pct * 127u) / 100u);
}

/**
 * @brief Play one burst of the selected haptic pattern at the given amplitude.
 *        Sets RTP amplitude first (escalation), then fires each effect step.
 *        Blocks for the duration of the burst (sum of step durations).
 */
static void play_alarm_burst(const alarm_haptic_pattern_t *pat, uint8_t amp_7bit)
{
    // Set RTP amplitude for intensity escalation
    haptic_set_rtp_amp(amp_7bit);

    // Play each step in the burst
    for (uint8_t i = 0; i < pat->step_count; i++) {
        if (pat->steps[i].effect_id > 0) {
            haptic_play_forced(pat->steps[i].effect_id);
        }
        vTaskDelay(pdMS_TO_TICKS(pat->steps[i].duration_ms));
    }
}

// ---------------------------------------------------------------------------
// Core 0 task — Stage A (NVS/timestamp) + Stage C (time-match/firing)
// ---------------------------------------------------------------------------

void task_alarm_fn(void *arg)
{
    (void)arg;
    const TickType_t period = pdMS_TO_TICKS(1000);
    TickType_t last = xTaskGetTickCount();

    // Guard: prevent re-fire during the same minute
    uint16_t last_fired_mod = 0xFFFF;

    // Firing state — local tracking alongside broker source of truth
    bool           local_firing       = false;
    uint32_t       firing_start_sec   = 0;
    uint8_t        firing_pattern_id  = 0;
    bool           firing_led_strobe  = false;          // active for this firing
    ws2812_state_t led_state_pre_fire = WS2812_STATE_OFF; // restored on dismiss

    ESP_LOGI(TAG, "Alarm task started (1 Hz poll, Stage C active)");

    while (1) {
        vTaskDelayUntil(&last, period);

        // Must have RTC to do anything useful
        if (!broker_rtc_hw_alive()) continue;

        broker_rtc_data_t   rtc = {0};
        broker_alarm_data_t ad  = {0};
        broker_rtc_read(&rtc);
        broker_alarm_read(&ad);

        if (!rtc.valid) {
            // Refresh broker timestamp to prevent STALE
            broker_alarm_write(&ad);
            continue;
        }

        // uint16_t cur_mod = (uint16_t)(rtc.hour * 60u + rtc.minute);

        // NEW — convert broker UTC hour to local before comparison.
        // Alarm slots store local time (user sets "07:30" meaning their local 07:30).
        // Broker now stores UTC (Phase 15 fix). Must apply offset here.
        int local_h = ((int)rtc.hour + (int)g_tz_offset_hours + 24) % 24;
        uint16_t cur_mod = (uint16_t)((uint8_t)local_h * 60u + rtc.minute);

        uint32_t now_sec = (uint32_t)(esp_timer_get_time() / 1000000ULL);

        // -- Detect external stop (Core 1 set firing=false via STOP button) --
        if (local_firing && !ad.firing) {
            local_firing = false;
            ESP_LOGI(TAG, "Firing stopped externally (STOP pressed)");
            alarm_nvs_clear_snooze();
            if (firing_led_strobe) {
                ws2812_set_state(led_state_pre_fire);
                firing_led_strobe = false;
            }
        }

        // -- Currently firing: manage escalation + auto-dismiss ---------------
        if (ad.firing && local_firing) {
            uint32_t elapsed = now_sec - firing_start_sec;

            if (elapsed >= ALARM_AUTO_DISMISS_SEC) {
                // Auto-dismiss after timeout
                ESP_LOGI(TAG, "Auto-dismiss after %lu sec", (unsigned long)elapsed);
                broker_alarm_data_t wr = {0};
                broker_alarm_read(&wr);
                wr.firing       = false;
                wr.snoozed      = false;
                wr.snooze_until = 0;
                broker_alarm_write(&wr);

                ui_event_t evt = { .type = UI_EVENT_NOTIF_DISMISS };
                ui_event_send(&evt);

                alarm_nvs_clear_snooze();
                local_firing = false;
                if (firing_led_strobe) {
                    ws2812_set_state(led_state_pre_fire);
                    firing_led_strobe = false;
                }
            } else {
                // Play one burst cycle with escalation
                uint8_t pid = firing_pattern_id;
                if (pid >= ALARM_PATTERN_COUNT) pid = 0;
                const alarm_haptic_pattern_t *pat = &k_patterns[pid];

                uint8_t amp = compute_intensity_7bit(elapsed);
                ESP_LOGD(TAG, "Burst: elapsed=%lus amp=%u/127 pat=%u",
                         (unsigned long)elapsed, (unsigned)amp, (unsigned)pid);
                play_alarm_burst(pat, amp);

                // Pause between bursts
                vTaskDelay(pdMS_TO_TICKS(pat->pause_ms));
            }

            // Refresh broker timestamp
            broker_alarm_read(&ad);
            broker_alarm_write(&ad);
            continue;
        }

        // -- Snooze re-fire check ---------------------------------------------
        if (ad.snoozed && !ad.firing && cur_mod == ad.snooze_until) {
            ESP_LOGI(TAG, "Snooze re-fire at minute %u", (unsigned)cur_mod);

            broker_alarm_data_t wr = {0};
            broker_alarm_read(&wr);
            wr.firing       = true;
            wr.snoozed      = false;
            wr.snooze_until = 0;
            broker_alarm_write(&wr);

            alarm_nvs_clear_snooze();

            ui_event_t evt = {
                .type = UI_EVENT_ALARM_FIRED,
                .payload.alarm = {
                    .alarm_id       = wr.firing_slot,
                    .snooze_minutes = ALARM_SNOOZE_MINUTES,
                },
            };
            ui_event_send(&evt);

            local_firing      = true;
            firing_start_sec  = now_sec;
            firing_pattern_id = (wr.firing_slot < ALARM_MAX_SLOTS)
                                ? wr.slots[wr.firing_slot].pattern_id : 0;
            last_fired_mod    = cur_mod;

            // LED strobe (Phase 6) — capture pre-fire state, switch to ALERT.
            firing_led_strobe = (wr.firing_slot < ALARM_MAX_SLOTS)
                                 && wr.slots[wr.firing_slot].led_strobe;
            if (firing_led_strobe) {
                led_state_pre_fire = ws2812_get_state();
                ws2812_set_state(WS2812_STATE_ALERT);
            }
            continue;
        }

        // -- Normal time-match scan -------------------------------------------
        // Clear the re-fire guard when minute changes
        if (last_fired_mod != 0xFFFF && cur_mod != last_fired_mod) {
            last_fired_mod = 0xFFFF;
        }

        if (last_fired_mod == cur_mod) {
            // Already fired this minute — skip scan
            broker_alarm_write(&ad);
            continue;
        }

        // In task_alarm_fn, around the normal time-match scan section.
        // Add a bool before the for-loop and use it to skip the stale write.

        // REPLACE this block (from "for (uint8_t i = 0;" through the final "broker_alarm_write(&ad);"):

        bool fired_this_tick = false;

        for (uint8_t i = 0; i < ALARM_MAX_SLOTS; i++) {
            if (ad.slots[i].hour == ALARM_SLOT_EMPTY) continue;
            if (!ad.slots[i].armed) continue;

            uint16_t slot_mod = (uint16_t)(ad.slots[i].hour * 60u +
                                           ad.slots[i].minute);
            if (slot_mod != cur_mod) continue;

            // Match found — fire!
            ESP_LOGI(TAG, "ALARM FIRED: slot %u  %02u:%02u  pattern=%u",
                     (unsigned)i, (unsigned)ad.slots[i].hour,
                     (unsigned)ad.slots[i].minute,
                     (unsigned)ad.slots[i].pattern_id);

            broker_alarm_data_t wr = {0};
            broker_alarm_read(&wr);
            wr.firing      = true;
            wr.firing_slot = i;
            broker_alarm_write(&wr);

            ui_event_t evt = {
                .type = UI_EVENT_ALARM_FIRED,
                .payload.alarm = {
                    .alarm_id       = i,
                    .snooze_minutes = ALARM_SNOOZE_MINUTES,
                },
            };
            ui_event_send(&evt);

            local_firing      = true;
            firing_start_sec  = now_sec;
            firing_pattern_id = ad.slots[i].pattern_id;
            last_fired_mod    = cur_mod;
            fired_this_tick   = true;

            // LED strobe (Phase 6) — capture pre-fire state, switch to ALERT.
            firing_led_strobe = ad.slots[i].led_strobe;
            if (firing_led_strobe) {
                led_state_pre_fire = ws2812_get_state();
                ws2812_set_state(WS2812_STATE_ALERT);
            }
            break;  // first match wins
        }

        // Refresh broker timestamp — but NOT if we just fired
        // (wr already wrote the authoritative state with firing=true)
        if (!fired_this_tick) {
            broker_alarm_write(&ad);
        }
    }
}