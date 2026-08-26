/**
 * @file field_capture.c
 * @brief Field-capture orchestrator -- main task + init + public getters.
 *
 * Stage 12 refactor split field_capture.c from 3.8k lines into:
 *   fc_common.c        -- shared globals, palette, mode taxonomy, button,
 *                         encoder, wake/park, RGB base helpers, NVS mode/boot,
 *                         SD mount + rtc_iso_now
 *   fc_recording.c     -- WAV + CSV writers, run_recording_for_current_mode,
 *                         voice_annot_lead_in, alarm firing
 *   fc_modes.c         -- Compass, ECG (inline QVAR), TEMP, mode-signature
 *                         LED animations (rgb_compass_alt_red_blue etc.)
 *   fc_modes_lsm.c     -- BCG, Steps, MLC_COLLECT, TAP_DBG
 *   fc_battery_test.c  -- batt_test mode + BLACKBOX task + esp_ts / vbat_adc /
 *                         idle_pct shared telemetry helpers
 *   fc_shutdown.c      -- watcher_ship_mode + task_shutdown_watcher_fn +
 *                         g_shutdown_hold_active / g_recording_active / etc.
 *   fc_cli.c           -- every rtc_cli_* + task_rtc_cli_fn
 *   fc_internal.h      -- shared decls, constants, types
 *
 * This file now owns only:
 *   - field_capture_init  (GPIO config, NVS load, ensure_sd)
 *   - field_capture_get_boot_seq  (public accessor)
 *   - task_field_capture_fn       (main FCM state machine loop)
 *
 * ═════════════════════════════════════════════════════════════════════════════
 * SD durability rule (added 2026-07-24 after batt_0025.csv came back empty).
 *
 * fflush() alone is NOT enough. It flushes stdio buffer to VFS but FatFs's
 * directory entry (file size + mtime) only gets committed on f_sync (via
 * fsync(fd)) or f_close. If the device dies between fflushes -- as batt_0025
 * did -- the file appears on disk with size 0 and NO data is recoverable.
 *
 * Every writer that survives loss-of-power must pick ONE of these patterns:
 *
 *   PATTERN A -- open-append-close per row.
 *     Use for LOW rate writers (say <10 Hz). Simplest to reason about.
 *     Each fclose() commits the directory entry. Cost ~1-5 ms per row.
 *     Example: run_battery_test_mode (10 s cadence).
 *
 *   PATTERN B -- keep open, fsync per row.
 *     Use for HIGH rate writers (say >=10 Hz) that can't afford fopen/fclose
 *     overhead. Header written + fclose on entry; on each row: open("a"), or
 *     hold file open + fflush() + fsync(fileno(f)). fsync forces f_sync
 *     which commits the directory entry. Cost ~0.5-2 ms per fsync.
 *     Example: run_mlc_collect_mode (50 Hz).
 *
 *   NEVER USE: fflush() alone. Under power loss, expect zero-byte files.
 * ═════════════════════════════════════════════════════════════════════════════
 */

#include "fc_internal.h"

#include "driver/gpio.h"
#include "esp_log.h"

#include "lsm6dsv16x.h"
#include "flashlight.h"
#include "sdcard.h"
#include "usb_msc.h"
#include "haptic.h"
#include "nvs_cfg.h"

static const char *TAG = "FIELD";

// ── Public accessor -- boot_seq stored in fc_common.c ───────────────────────
uint32_t field_capture_get_boot_seq(void) {
    return s_boot_seq;
}

// Callback for usb_msc_run_until_exit -- returns true on a single click.
// Runs from the usb_msc loop tick (~50 ms), not from the main state machine.
static bool usb_msc_button_click_cb(void) {
    return button_poll() == 1;
}

// ── Public init ──────────────────────────────────────────────────────────────
void field_capture_init(void) {
    static bool inited = false;
    if (inited) return;
    inited = true;

    // Button + encoder pins: input, internal pull-up, no ISR (all polled).
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << PIN_BUTTON) |
                        (1ULL << PIN_ENC_A)  |
                        (1ULL << PIN_ENC_B),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
    s_btn_prev_low = (gpio_get_level(PIN_BUTTON) == 0);

    nvs_load();
    s_last_activity_ms = millis_u32();
    ensure_sd();
    ESP_LOGI(TAG, "field_capture_init OK (mode=%s boot_seq=%lu)",
             MODE_INFO[s_mode].name, (unsigned long)s_boot_seq);
}

// ── Main task loop ───────────────────────────────────────────────────────────
void task_field_capture_fn(void *arg) {
    (void)arg;
    field_capture_init();

    // Battery-test mode: NVS-flagged one-way boot into a 10 s cadence logger.
    // Never returns from run_battery_test_mode() -- device runs until BQ25619
    // cuts power at UVLO. Ship-mode gesture (4 s button hold) is the manual
    // escape via task_shutdown_watcher_fn.
    if (nvs_cfg_sys_get_batt_test()) {
        run_battery_test_mode();
        // unreachable
    }

    for (;;) {
        int btn = button_poll();
        int enc = encoder_delta();

        // ── Stage 10 "go back" gesture: LSM tap-double OR button double ──
        // Only acts while ST_STANDBY -- if a recording is live we don't
        // want a stray gesture to abort the session. Tap counter is polled
        // + deduped on strict increase; button double-click is btn==2. Both
        // do the same thing so the user has a fallback while tap thresholds
        // are being tuned.
        uint32_t tap_dbl = lsm6dsv16x_tap_z_double_count();
        bool exit_submenu = false;
        const char *exit_reason = NULL;
        if (tap_dbl != s_last_tap_dbl_count) {
            s_last_tap_dbl_count = tap_dbl;
            exit_submenu = true;
            exit_reason  = "tap-double";
        }
        if (btn == 2 && s_state == ST_STANDBY && s_in_submenu) {
            exit_submenu = true;
            exit_reason  = "button-double (tap backup)";
            btn = 0;  // consume so the ST_STANDBY switch doesn't also see it
        }
        if (exit_submenu && s_state == ST_STANDBY && s_in_submenu) {
            s_in_submenu = false;
            s_mode = FCM_LSM;
            nvs_save_mode();
            haptic_play(DRV_MEDIUM_CLICK);
            s_last_activity_ms = millis_u32();
            ESP_LOGI(TAG, "LSM submenu EXIT (%s)", exit_reason);
        }

        switch (s_state) {

        case ST_STANDBY: {
            if (enc != 0) {
                if (s_in_submenu) {
                    s_mode = lsm_submenu_step(s_mode, enc);
                } else {
                    s_mode = top_mode_step(s_mode, enc);
                }
                nvs_save_mode();
                haptic_play(DRV_MEDIUM_CLICK);
                s_last_activity_ms = millis_u32();
                ESP_LOGI(TAG, "mode -> %s (%s)", MODE_INFO[s_mode].name,
                         s_in_submenu ? "LSM sub" : "top");
            }
            if (btn == 1) {
                if (!s_in_submenu && s_mode == FCM_LSM) {
                    // Enter the LSM submenu. Land on the first submenu entry.
                    s_in_submenu = true;
                    s_mode = FC_LSM_SUBMENU[0];
                    nvs_save_mode();
                    haptic_play(DRV_STRONG_CLICK);
                    s_last_activity_ms = millis_u32();
                    ESP_LOGI(TAG, "LSM submenu ENTER (mode -> %s)",
                             MODE_INFO[s_mode].name);
                } else if (s_mode == FCM_FLASHLIGHT) {
                    flashlight_set_brightness(fl_pct_from_level(s_fl_level));
                    s_state = ST_FL_ON;
                } else if (s_mode == FCM_ALARM) {
                    s_state = ST_ALARM_FIRING;
                    run_alarm_firing();
                    s_state = ST_STANDBY;
                } else if (s_mode == FCM_COMPASS) {
                    run_compass_or_cal();
                    s_state = ST_STANDBY;
                } else if (s_mode == FCM_ECG) {
                    run_ecg_session();
                    s_state = ST_STANDBY;
                } else if (s_mode == FCM_TEMP) {
                    run_temp_mode();
                    s_state = ST_STANDBY;
                } else if (s_mode == FCM_BCG) {
                    run_bcg_mode();
                    s_state = ST_STANDBY;
                } else if (s_mode == FCM_PPG_BCG) {
                    // Stage 17: combined PPG+BCG raw at 200 Hz with pc_sync_*
                    // CSV headers (Stage 15 spec). Bespoke tick loop -- not
                    // routed through run_recording_for_current_mode().
                    s_state = ST_RECORDING;
                    run_ppg_bcg_mode();
                    s_state = ST_STANDBY;
                } else if (s_mode == FCM_STEPS) {
                    run_steps_mode();
                    s_state = ST_STANDBY;
                } else if (s_mode == FCM_MLC_COLLECT) {
                    run_mlc_collect_mode();
                    s_state = ST_STANDBY;
                } else if (s_mode == FCM_TAP_DBG) {
                    run_tap_dbg_mode();
                    s_state = ST_STANDBY;
                } else if (s_mode == FCM_USB_MSC) {
                    // Close any recording sessions cleanly before handing the
                    // SD host to USB. usb_msc_run_until_exit() reboots on
                    // click / unplug, so this call typically does not return.
                    (void)sdcard_close_session();
                    s_sd_ready = false;
                    ESP_LOGW(TAG, "Entering FCM_USB_MSC -- device will reboot "
                                  "on exit (button click or USB unplug).");
                    (void)usb_msc_run_until_exit(usb_msc_button_click_cb);
                    s_state = ST_STANDBY;
                } else {
                    s_state = ST_RECORDING;
                    run_recording_for_current_mode();
                    s_state = ST_STANDBY;
                }
                s_last_activity_ms = millis_u32();
            }
            // LED animation. Skip entirely while the shutdown watcher is
            // running its hold-countdown -- watcher owns the LED then.
            if (!g_shutdown_hold_active) {
                if (s_in_submenu) {
                    rgb_lsm_submenu_indicator(s_mode);
                } else if (s_mode == FCM_COMPASS) {
                    rgb_compass_alt_red_blue();
                } else if (s_mode == FCM_ECG) {
                    rgb_qvar_alt_yellow_purple();
                } else if (s_mode == FCM_TEMP) {
                    rgb_temp_warm_cycle();
                } else {
                    uint32_t solid_end = s_last_activity_ms + SOLID_ON_ACTIVITY_MS;
                    if (millis_u32() < solid_end) rgb_set_max(s_mode);
                    else                          rgb_pulse(s_mode, solid_end, PULSE_STANDBY_MS);
                }
            }
            break;
        }

        case ST_FL_ON: {
            if (enc != 0) {
                int nl = (int)s_fl_level + enc;
                if (nl < 0)          nl = 0;
                if (nl > FL_LEVELS)  nl = FL_LEVELS;
                s_fl_level = (uint8_t)nl;
                flashlight_set_brightness(fl_pct_from_level(s_fl_level));
                haptic_play(DRV_STRONG_CLICK);
            }
            if (btn == 1) {
                flashlight_off();
                s_state = ST_STANDBY;
                s_last_activity_ms = millis_u32();
            }
            rgb_set_max(FCM_FLASHLIGHT);
            break;
        }

        case ST_RECORDING:
        case ST_ALARM_FIRING:
            // Handled by their blocking helpers above; state is restored to
            // STANDBY on their return. This branch shouldn't be reached in
            // normal flow.
            s_state = ST_STANDBY;
            break;
        }

        vTaskDelay(pdMS_TO_TICKS(POLL_TICK_MS));
    }
}
