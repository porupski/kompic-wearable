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
 *   fc_battery_test.c  -- batt_test mode + BLACKBOX task + esp_ts /
 *                         fuel-gauge snapshot / idle_pct shared telemetry
 *                         helpers
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
#include "rgb_policy.h"

static const char *TAG = "FIELD";

// ── Public accessor -- boot_seq stored in fc_common.c ───────────────────────
uint32_t field_capture_get_boot_seq(void) {
    return s_boot_seq;
}

void field_capture_kick_activity(void) {
    s_last_activity_ms = millis_u32();
    // Also postpone the auto-shutdown uptime cap. Any surface that kicks
    // activity (touch press-edge, external caller) then covers both the
    // display-idle timer and the auto-shdn deadline in one call.
    shutdown_watcher_kick();
}

// ── Cross-core back-gesture signal (Stage 28 §4.4) ──────────────────────────
// Any thread can request a "back" event by bumping s_back_req_count. The main
// loop polls the delta each tick, evaluates current state, and dispatches to
// wake / submenu-exit / sleep. Reason string stashed in a small ring so we
// can log which source fired even if two requests land in the same tick.
#define BACK_REASON_RING_LEN 4
static volatile uint32_t s_back_req_count            = 0;
static uint32_t          s_back_req_seen             = 0;
static const char       *s_back_reasons[BACK_REASON_RING_LEN] = {0};
static volatile uint32_t s_back_reason_wr            = 0;

void field_capture_back_gesture(const char *reason) {
    uint32_t idx = __atomic_fetch_add(&s_back_reason_wr, 1, __ATOMIC_RELAXED);
    s_back_reasons[idx % BACK_REASON_RING_LEN] = reason ? reason : "?";
    __atomic_fetch_add(&s_back_req_count, 1, __ATOMIC_RELAXED);
}

static const char *back_reason_take(void) {
    uint32_t wr = __atomic_load_n(&s_back_reason_wr, __ATOMIC_RELAXED);
    if (wr == 0) return "?";
    return s_back_reasons[(wr - 1) % BACK_REASON_RING_LEN];
}

// Callback for usb_msc_run_until_exit -- returns true on a single click.
// Runs from the usb_msc loop tick (~50 ms), not from the main state machine.
static bool usb_msc_button_click_cb(void) {
    return button_poll() == 1;
}

// ── Public init ──────────────────────────────────────────────────────────────
void field_capture_init(void) {
    ESP_LOGI(TAG, "driver v%s", FIELD_CAPTURE_DRIVER_VERSION);
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

    // --- TEMP HARDCODE 2026-09-16: encoder dead, force FLASHLIGHT boot ---
    // Does NOT call nvs_save_mode(), so the persisted mode in NVS is
    // untouched -- delete this block once the encoder is fixed and the
    // device will resume remembering mode normally.
    s_mode = FCM_FLASHLIGHT;
    s_in_submenu = false;
    // --- END TEMP HARDCODE ---
    s_last_activity_ms = millis_u32();
    ensure_sd();

    // Seed rgb_policy with the loaded mode's palette colour so the idle
    // pulse breathes in the right colour before the user touches the
    // encoder. Without this the LED breathes soft grey until first
    // encoder rotation.
    {
        const mode_info_t *mi = &MODE_INFO[s_mode];
        rgb_policy_set_mode_color(mi->r, mi->g, mi->b);
    }

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

        // ── Display auto-sleep: 30 s of no button / encoder / mode-change
        //    activity puts the panel into DISPOFF+SLPIN. Button single-click
        //    wakes (button_poll consumes the click). No-op when the panel is
        //    absent or already asleep. Bumped 15 s -> 30 s per Ivan
        //    2026-09-15 (Stage 30 §4.2h) -- 15 s was too aggressive during
        //    on-panel diagnostic reads.
        {
            extern bool      boot_display_is_present(void);
            extern bool      boot_display_is_asleep(void);
            extern esp_err_t boot_display_sleep(void);
            const uint32_t DISP_IDLE_MS = 30000;
            uint32_t now_ms = millis_u32();
            if (boot_display_is_present() && !boot_display_is_asleep() &&
                s_state == ST_STANDBY &&
                (now_ms - s_last_activity_ms) >= DISP_IDLE_MS) {
                ESP_LOGI(TAG, "[DISP] reason=idle-timeout (%u ms idle)",
                         (unsigned)DISP_IDLE_MS);
                (void)boot_display_sleep();
            }
        }

        // ── Universal back-gesture dispatch (Stage 28 §4.4) ──────────────
        // btn double-click, LSM tap-double, and CST9217 touch double-tap all
        // funnel through field_capture_back_gesture() into a single atomic
        // request counter. Semantics: asleep -> wake; in-submenu -> up one
        // level; top-level -> lock (sleep). Ivan's spec 2026-09-13:
        // "double tap should exit the menu, go up a level, and if highest
        // level, then lock. double tap locks the screen anytime anywhere."
        {
            // btn==2 while ST_STANDBY: fold into the back-gesture queue.
            if (btn == 2 && s_state == ST_STANDBY) {
                field_capture_back_gesture("btn-double");
                btn = 0;   // swallow so no downstream mode action fires
            }
            // LSM tap-double: dedupe on strict increase, same queue.
            uint32_t tap_dbl = lsm6dsv16x_tap_z_double_count();
            if (tap_dbl != s_last_tap_dbl_count) {
                s_last_tap_dbl_count = tap_dbl;
                if (s_state == ST_STANDBY) {
                    field_capture_back_gesture("lsm-tap-double");
                }
            }

            // Drain the shared back counter -- one dispatch per bump.
            uint32_t req_now = __atomic_load_n(&s_back_req_count,
                                               __ATOMIC_RELAXED);
            while (req_now != s_back_req_seen) {
                s_back_req_seen++;
                const char *reason = back_reason_take();
                extern bool      boot_display_is_present(void);
                extern bool      boot_display_is_asleep(void);
                extern esp_err_t boot_display_wake(void);
                extern esp_err_t boot_display_sleep(void);
                extern bool      ui_navigation_is_on_main(void);
                if (boot_display_is_present() && boot_display_is_asleep()) {
                    ESP_LOGI(TAG, "[DISP] back-gesture reason=%s -> wake",
                             reason);
                    (void)boot_display_wake();
                    s_last_activity_ms = millis_u32();
                } else if (s_state == ST_STANDBY && s_in_submenu) {
                    ESP_LOGI(TAG, "[BACK] reason=%s -> submenu exit",
                             reason);
                    s_in_submenu = false;
                    s_mode = FCM_LSM;
                    nvs_save_mode();
                    haptic_play(DRV_MEDIUM_CLICK);
                    s_last_activity_ms = millis_u32();
                } else if (s_state == ST_STANDBY &&
                           boot_display_is_present() &&
                           ui_navigation_is_on_main()) {
                    // Stage 28 §4.5: lock branch now narrows to main-screen
                    // only. Ivan's bench 2026-09-13: accidental double taps
                    // while browsing Settings tiles kept sleeping the
                    // panel. On non-main screens the back gesture is a
                    // no-op (swipe-down is the intended nav-back path).
                    ESP_LOGI(TAG, "[DISP] back-gesture reason=%s -> sleep",
                             reason);
                    (void)boot_display_sleep();
                } else if (s_state == ST_STANDBY &&
                           boot_display_is_present()) {
                    ESP_LOGI(TAG, "[BACK] reason=%s -> ignored (not on main)",
                             reason);
                } else {
                    ESP_LOGI(TAG, "[BACK] reason=%s -> ignored (state=%d)",
                             reason, (int)s_state);
                }
            }
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
                // Stage 17 §3.3: 5 s preview window in the mode's palette
                // colour. rgb_policy overlays it on the idle LED background.
                {
                    const mode_info_t *mi = &MODE_INFO[s_mode];
                    rgb_policy_preview_start(mi->r, mi->g, mi->b);
                }
            }
            if (btn == 1) {
                // Stage 17 §3.3: hand the RGB to the mode until it returns.
                // Every branch below either enters ST_FL_ON (which resumes on
                // exit via its own path), never returns (USB_MSC), or is a
                // blocking mode function that owns the LED for its duration.
                rgb_policy_pause();
                if (!s_in_submenu && s_mode == FCM_LSM) {
                    // Submenu entry does not touch the LED. Resume the policy
                    // so the preview window on the new sub-mode still shows.
                    s_in_submenu = true;
                    s_mode = FC_LSM_SUBMENU[0];
                    nvs_save_mode();
                    haptic_play(DRV_STRONG_CLICK);
                    s_last_activity_ms = millis_u32();
                    ESP_LOGI(TAG, "LSM submenu ENTER (mode -> %s)",
                             MODE_INFO[s_mode].name);
                    {
                        const mode_info_t *mi = &MODE_INFO[s_mode];
                        rgb_policy_preview_start(mi->r, mi->g, mi->b);
                    }
                    rgb_policy_resume();
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
                // Stage 17 §3.3: mode returned. Hand the RGB back to policy
                // unless we entered ST_FL_ON, which owns the LED continuously
                // until the user clicks out (that path calls resume itself).
                if (s_state != ST_FL_ON) {
                    rgb_policy_resume();
                }
            }
            // Stage 17 §3.3: STANDBY LED is owned by rgb_policy (5 s preview
            // on mode select, otherwise off unless charging/alert/wrist-up).
            // The mode-specific standby animations (compass gradient, ECG
            // yellow/purple, temp fire strobe) intentionally go away here --
            // they surface via the 5 s preview instead.
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
                rgb_policy_resume();   // hand LED back to policy
            }
            // Flashlight mode owns the RGB (bright white) while active.
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
