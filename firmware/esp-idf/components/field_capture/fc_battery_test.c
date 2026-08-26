/**
 * @file fc_battery_test.c
 * @brief Battery-test mode, BLACKBOX telemetry task, shared telemetry helpers.
 *
 * Split out of field_capture.c in the Stage 12 refactor.
 *
 * Owns three related things kept together because they share the same helper
 * plumbing (esp_ts, vbat_adc, idle_pct):
 *   - Shared telemetry helpers (esp_ts / vbat_adc / idle_pct)
 *   - run_battery_test_mode() -- takes over the main task on NVS-flagged boot
 *   - task_blackbox_fn -- background CSV logger, boots regardless
 *
 * SD durability: PATTERN A (open-append-close per row) with SD unmount between
 * rows in BATT_TEST for PM light-sleep engagement.
 */

#include "fc_internal.h"
#include "firmware_version.h"

#include <string.h>
#include <errno.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_pm.h"
#include "soc/rtc.h"
#include "freertos/task.h"

#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "driver/temperature_sensor.h"

#include "data_broker.h"
#include "ws2812.h"
#include "sdcard.h"
#include "nvs_cfg.h"

static const char *TAG = "FC_BATT";

// ── Vbat ADC — 5k1-5k1 divider on GPIO18 (ADC2_CH7). Prototype override:
//    borrows the GPS UART RX pin because this unit will not have M10S fitted.
//    ADC2 shares the RF-cal path with Wi-Fi; safe here because Kompic is
//    BLE-only. If Wi-Fi is ever enabled, oneshot reads will start returning
//    ESP_ERR_TIMEOUT and this must move again. ─────────────────────────────
static adc_oneshot_unit_handle_t s_vbat_adc  = NULL;
static adc_cali_handle_t         s_vbat_cali = NULL;
static bool                      s_vbat_init_tried = false;

void vbat_adc_ensure_init(void)
{
    if (s_vbat_init_tried) return;
    s_vbat_init_tried = true;

    adc_oneshot_unit_init_cfg_t unit_cfg = {
        .unit_id  = ADC_UNIT_2,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    if (adc_oneshot_new_unit(&unit_cfg, &s_vbat_adc) != ESP_OK) {
        ESP_LOGW(TAG, "Vbat ADC: unit init failed");
        s_vbat_adc = NULL;
        return;
    }
    adc_oneshot_chan_cfg_t chan_cfg = {
        .bitwidth = ADC_BITWIDTH_DEFAULT,
        .atten    = ADC_ATTEN_DB_12,
    };
    adc_oneshot_config_channel(s_vbat_adc, ADC_CHANNEL_7, &chan_cfg);

    adc_cali_curve_fitting_config_t cali_cfg = {
        .unit_id  = ADC_UNIT_2,
        .chan     = ADC_CHANNEL_7,
        .atten    = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    if (adc_cali_create_scheme_curve_fitting(&cali_cfg, &s_vbat_cali) != ESP_OK) {
        ESP_LOGW(TAG, "Vbat ADC: curve-fit cali unavailable, using raw counts");
        s_vbat_cali = NULL;
    }
    ESP_LOGI(TAG, "Vbat ADC armed on GPIO18 (ADC2_CH7), 5k1-5k1 divider");
}

uint32_t vbat_adc_read_mv(void)
{
    if (!s_vbat_adc) return 0;
    int raw_acc = 0;
    int ok_n    = 0;
    for (int i = 0; i < 8; i++) {
        int raw;
        if (adc_oneshot_read(s_vbat_adc, ADC_CHANNEL_7, &raw) == ESP_OK) {
            raw_acc += raw;
            ok_n++;
        }
    }
    if (ok_n == 0) return 0;
    int raw_avg = raw_acc / ok_n;
    int mv_at_pin = 0;
    if (s_vbat_cali &&
        adc_cali_raw_to_voltage(s_vbat_cali, raw_avg, &mv_at_pin) == ESP_OK) {
        return (uint32_t)(mv_at_pin * 2);
    }
    return (uint32_t)((raw_avg * 3300 / 4095) * 2);
}

// ── ESP32-S3 SoC junction temperature ───────────────────────────────────────
static temperature_sensor_handle_t s_esp_ts = NULL;
static bool                        s_esp_ts_tried = false;

void esp_ts_ensure_init(void)
{
    if (s_esp_ts_tried) return;
    s_esp_ts_tried = true;
    temperature_sensor_config_t cfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
    if (temperature_sensor_install(&cfg, &s_esp_ts) != ESP_OK ||
        temperature_sensor_enable(s_esp_ts) != ESP_OK) {
        ESP_LOGW(TAG, "ESP32-S3 SoC temp install failed -- reads return -273");
        s_esp_ts = NULL;
    }
}

float esp_ts_read_c(void)
{
    if (!s_esp_ts) return -273.15f;
    float t = -273.15f;
    (void)temperature_sensor_get_celsius(s_esp_ts, &t);
    return t;
}

// ── idle_pct sampler (requires FREERTOS_GENERATE_RUN_TIME_STATS) ────────────
uint8_t idle_pct_sample(idle_pct_state_t *st)
{
#if CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
    uint32_t c   = ulTaskGetIdleRunTimeCounter();
    uint32_t now = (uint32_t)esp_timer_get_time();

    if (!st->primed) {
        st->last_idle_c0 = c;
        st->last_wall_us = now;
        st->primed = true;
        return 0;
    }

    uint32_t d_idle = c - st->last_idle_c0;
    uint32_t d_wall = now - st->last_wall_us;
    st->last_idle_c0 = c;
    st->last_wall_us = now;

    if (d_wall == 0) return 0;
    uint32_t pct = (uint32_t)((uint64_t)d_idle * 100U / (uint64_t)d_wall);
    if (pct > 100) pct = 100;
    return (uint8_t)pct;
#else
    (void)st;
    return 0;
#endif
}

// ═════════════════════════════════════════════════════════════════════════════
// BLACKBOX telemetry task (Stage 11).
// Always-on background CSV logger, NVS-flagged. Snapshots every broker + all
// system stats every cfg_sys.bb_cadence_s seconds. Pattern A durability.
// ═════════════════════════════════════════════════════════════════════════════

void task_blackbox_fn(void *arg)
{
    (void)arg;

    if (!nvs_cfg_sys_get_blackbox()) {
        ESP_LOGI(TAG, "BLACKBOX: disabled by NVS flag -- task exiting");
        vTaskDelete(NULL);
        return;
    }
    if (nvs_cfg_sys_get_batt_test()) {
        ESP_LOGI(TAG, "BLACKBOX: batt_test mode is active -- task exiting to avoid double-CSV");
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "BLACKBOX: task started on Core %d", xPortGetCoreID());

    for (int i = 0; i < 50 && !s_sd_ready; i++) {
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    if (!s_sd_ready) {
        ESP_LOGE(TAG, "BLACKBOX: SD never mounted -- task exiting");
        vTaskDelete(NULL);
        return;
    }
    try_mkdir("/sd/data/blackbox");

    esp_ts_ensure_init();
    vbat_adc_ensure_init();
    idle_pct_state_t idle_st = { 0 };

    char bb_path[80];
    snprintf(bb_path, sizeof(bb_path),
             "/sd/data/blackbox/bb_%04lu.csv",
             (unsigned long)s_boot_seq);
    {
        FILE *f = fopen(bb_path, "w");
        if (!f) {
            ESP_LOGE(TAG, "BLACKBOX: fopen %s failed: %s", bb_path, strerror(errno));
            vTaskDelete(NULL);
            return;
        }
        char now[32]; rtc_iso_now(now, sizeof(now));
        fprintf(f, "# kompic mk1 %s fw=%s hw=%s mode=blackbox boot=%lu rtc_start=%s\n",
                KOMPIC_HW_VERSION, KOMPIC_FW_VERSION, KOMPIC_HW_VERSION,
                (unsigned long)s_boot_seq, now);
        fprintf(f, "# sensors_on bitmap: IMU|MAG|ENV|LIGHT|HR|SKIN|BAT|RTC (LSB=IMU).\n");
        fprintf(f, "# sensors_stat: packed nibbles per sensor, same order. Values are sensor_status_t:\n");
        fprintf(f, "#   0=DISABLED 1=OFFLINE 2=ACQUIRING 3=STALE 4=ONLINE 5=NOTIF\n");
        fprintf(f, "# fcm_mode is the current top-level FCM enum name; fcm_state = STANDBY/FL_ON/RECORDING/ALARM.\n");
        fprintf(f, "# idle_pct: %% of wall-time both cores spent in Idle since previous row.\n");
        fprintf(f, "# batt_mv is the BQ25619 threshold register -- IGNORE; use vbat_adc_mv.\n");
        fprintf(f, "t_ms,iso_utc,uptime_min,boot_seq,");
        fprintf(f, "fcm_mode,fcm_state,");
        fprintf(f, "heap_free_kb,min_heap_kb,cpu_mhz,idle_pct,sensors_on,sensors_stat,");
        fprintf(f, "bq_v,bq_pct,bq_chg,bq_pg,bq_fault,vbat_adc_mv,soc_temp_c,");
        fprintf(f, "imu_ax,imu_ay,imu_az,imu_gx,imu_gy,imu_gz,imu_temp,");
        fprintf(f, "mag_x,mag_y,mag_z,");
        fprintf(f, "env_t,env_h,env_p,env_gas,env_alt,");
        fprintf(f, "light_lux,");
        fprintf(f, "hr_bpm,hr_spo2,hr_finger,");
        fprintf(f, "skin_t\n");
        fclose(f);
        ESP_LOGI(TAG, "BLACKBOX: logging to %s (open-append-close per row)", bb_path);
    }

    static const char *st_names[] = {"STANDBY", "FL_ON", "RECORDING", "ALARM"};

    uint32_t next_ms  = millis_u32();
    uint32_t idx      = 0;

    for (;;) {
        uint32_t cadence_ms = (uint32_t)nvs_cfg_sys_get_bb_cadence_s() * 1000U;
        uint32_t now = millis_u32();

        if ((int32_t)(now - next_ms) < 0) {
            uint32_t wait = next_ms - now;
            if (wait > 1000) wait = 1000;
            vTaskDelay(pdMS_TO_TICKS(wait));
            continue;
        }

        char iso[32]; rtc_iso_now(iso, sizeof(iso));
        uint32_t uptime_min = now / 60000U;

        uint32_t heap_free_kb = esp_get_free_heap_size() / 1024;
        uint32_t min_heap_kb  = esp_get_minimum_free_heap_size() / 1024;
        rtc_cpu_freq_config_t cpu_cfg;
        rtc_clk_cpu_freq_get_config(&cpu_cfg);
        uint32_t cpu_mhz = cpu_cfg.freq_mhz;
        uint8_t  idle_pct = idle_pct_sample(&idle_st);

        uint32_t sensors_on = 0;
        if (broker_imu_get_enabled())     sensors_on |= (1 << 0);
        if (broker_mag_get_enabled())     sensors_on |= (1 << 1);
        if (broker_env_get_enabled())     sensors_on |= (1 << 2);
        if (broker_light_get_enabled())   sensors_on |= (1 << 3);
        if (broker_hr_get_enabled())      sensors_on |= (1 << 4);
        if (broker_skin_get_enabled())    sensors_on |= (1 << 5);
        if (broker_battery_get_enabled()) sensors_on |= (1 << 6);
        if (broker_rtc_get_enabled())     sensors_on |= (1 << 7);

        uint32_t stat = 0;
        stat |= ((uint32_t)(broker_imu_get_status()     & 0x0F) << (0  * 4));
        stat |= ((uint32_t)(broker_mag_get_status()     & 0x0F) << (1  * 4));
        stat |= ((uint32_t)(broker_env_get_status()     & 0x0F) << (2  * 4));
        stat |= ((uint32_t)(broker_light_get_status()   & 0x0F) << (3  * 4));
        stat |= ((uint32_t)(broker_hr_get_status()      & 0x0F) << (4  * 4));
        stat |= ((uint32_t)(broker_skin_get_status()    & 0x0F) << (5  * 4));
        stat |= ((uint32_t)(broker_battery_get_status() & 0x0F) << (6  * 4));
        stat |= ((uint32_t)(broker_rtc_get_status()     & 0x0F) << (7  * 4));

        broker_battery_data_t bat; broker_battery_read(&bat);
        broker_imu_data_t     im;  broker_imu_read(&im);
        broker_mag_data_t     mg;  broker_mag_read(&mg);
        broker_env_data_t     en;  broker_env_read(&en);
        broker_light_data_t   li;  broker_light_read(&li);
        broker_hr_data_t      hr;  broker_hr_read(&hr);
        broker_skin_data_t    sk;  broker_skin_read(&sk);

        float t_soc = esp_ts_read_c();
        uint32_t vbat_adc_mv = vbat_adc_read_mv();

        const char *mode_name = MODE_INFO[s_mode].name;
        const char *state_name = (s_state < (int)(sizeof(st_names)/sizeof(*st_names)))
                                  ? st_names[s_state] : "?";

        FILE *f = fopen(bb_path, "a");
        if (f) {
            fprintf(f, "%lu,%s,%lu,%lu,%s,%s,",
                    (unsigned long)now, iso,
                    (unsigned long)uptime_min, (unsigned long)s_boot_seq,
                    mode_name, state_name);
            fprintf(f, "%lu,%lu,%lu,%u,0x%02lX,0x%08lX,",
                    (unsigned long)heap_free_kb, (unsigned long)min_heap_kb,
                    (unsigned long)cpu_mhz, (unsigned)idle_pct,
                    (unsigned long)sensors_on, (unsigned long)stat);
            fprintf(f, "%.3f,%u,%u,%u,0x%02X,%lu,%.2f,",
                    bat.voltage, (unsigned)bat.percentage,
                    (unsigned)(bat.charging ? 1 : 0),
                    (unsigned)(bat.power_good ? 1 : 0),
                    (unsigned)bat.fault,
                    (unsigned long)vbat_adc_mv, t_soc);
            fprintf(f, "%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.1f,",
                    im.accel_x, im.accel_y, im.accel_z,
                    im.gyro_x, im.gyro_y, im.gyro_z, im.temperature);
            fprintf(f, "%.2f,%.2f,%.2f,",
                    mg.x_ut, mg.y_ut, mg.z_ut);
            fprintf(f, "%.2f,%.2f,%.2f,%.0f,%.1f,",
                    en.temperature_c, en.humidity_pct, en.pressure_hpa,
                    en.gas_resistance_ohm, en.altitude_m);
            fprintf(f, "%.1f,", li.lux);
            fprintf(f, "%u,%.1f,%u,",
                    (unsigned)hr.bpm, hr.spo2_pct,
                    (unsigned)(hr.finger_detected ? 1 : 0));
            fprintf(f, "%.2f\n", sk.skin_temp_c);
            fclose(f);
        } else {
            ESP_LOGW(TAG, "BLACKBOX: append fopen failed: %s (SD unmounted?)",
                     strerror(errno));
        }

        if ((idx % 6) == 0) {
            ESP_LOGI(TAG, "BLACKBOX #%lu  %s  mode=%s  heap=%luKB  idle=%u%%  sens=0x%02lX",
                     (unsigned long)idx, iso, mode_name,
                     (unsigned long)heap_free_kb, (unsigned)idle_pct,
                     (unsigned long)sensors_on);
        }

        idx++;
        next_ms += cadence_ms;
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// BATTERY TEST mode -- boot-flagged, replaces the normal FCM state machine.
// 10 s cadence, unmounts SD between rows for light-sleep engagement, VBUS-
// aware PM lock so USB stays interactive when plugged.
// ═════════════════════════════════════════════════════════════════════════════

void run_battery_test_mode(void) {
    g_batt_test_active = true;

    ESP_LOGW(TAG, "BATTERY TEST MODE active. 15-min uptime cap suppressed. "
                  "Sample cadence: 10 s. Sensors parked. To exit: reboot with "
                  "USB attached and run BATT_TEST OFF.");

    ensure_sd();
    if (!s_sd_ready) {
        ESP_LOGE(TAG, "BATT_TEST: SD not mounted -- CSV logging disabled. "
                      "Battery run continues but no data will be recorded.");
    } else {
        try_mkdir("/sd/data/battery");
    }

    esp_ts_ensure_init();
    vbat_adc_ensure_init();
    idle_pct_state_t idle_st = { 0 };

    esp_pm_lock_handle_t vbus_lock = NULL;
    bool                 vbus_lock_held = false;
#ifdef CONFIG_PM_ENABLE
    if (esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0,
                            "batt_test_vbus", &vbus_lock) != ESP_OK) {
        ESP_LOGW(TAG, "BATT_TEST: PM lock create failed -- serial may drop on VBUS");
        vbus_lock = NULL;
    }
#endif

    char batt_path[80];
    bool batt_path_ok = false;
    if (s_sd_ready) {
        snprintf(batt_path, sizeof(batt_path),
                 "/sd/data/battery/batt_%04lu.csv",
                 (unsigned long)s_boot_seq);
        FILE *f = fopen(batt_path, "w");
        if (f) {
            char now[32]; rtc_iso_now(now, sizeof(now));
            fprintf(f, "# kompic mk1 %s fw=%s hw=%s mode=battery_test "
                       "boot=%lu rtc_start=%s\n",
                    KOMPIC_HW_VERSION, KOMPIC_FW_VERSION, KOMPIC_HW_VERSION,
                    (unsigned long)s_boot_seq, now);
            fprintf(f, "# sample every 10 s until BQ25619 UVLO cutoff\n");
            fprintf(f, "# batt_mv is a BQ25619 threshold register (not a real ADC), ignore it.\n");
            fprintf(f, "# vbat_adc_mv is a temporary 5k1-5k1 divider on GPIO9 (screen D2 pad) -- ~+/-1%% accuracy.\n");
            fprintf(f, "# cpu_mhz is snapshot at sample time (DFS makes it vary; sample is during work window -> usually 240).\n");
            fprintf(f, "# sensors_on: bitmap IMU|MAG|ENV|LIGHT|HR|SKIN|BAT|RTC (LSB=IMU). All 0 during batt_test except BAT+RTC which are always-on.\n");
            fprintf(f, "# idle_pct: %% of wall-time both cores spent in Idle since previous row. High = PM light-sleep engaging.\n");
            fprintf(f, "t_ms,iso_utc,soc_temp_c,batt_mv,batt_pct,charging,vbat_adc_mv,heap_free_kb,cpu_mhz,sensors_on,idle_pct\n");
            fclose(f);
            batt_path_ok = true;
            ESP_LOGI(TAG, "BATT_TEST: logging to %s (open-append-close per row, SD unmounted between samples)", batt_path);
        } else {
            ESP_LOGE(TAG, "BATT_TEST: fopen %s failed: %s", batt_path, strerror(errno));
        }
        (void)sdcard_unmount();
        s_sd_ready = false;
    }

    const uint32_t PERIOD_MS  = 10000;
    const uint32_t LED_BLIP_MS = 50;
    uint32_t next_sample_ms = millis_u32();
    uint32_t sample_idx     = 0;

    for (;;) {
        uint32_t now = millis_u32();
        if ((int32_t)(now - next_sample_ms) < 0) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        ws2812_set_color(0, WS_MAX, WS_MAX);

        float t_soc = esp_ts_read_c();
        uint8_t idle_pct = idle_pct_sample(&idle_st);

        broker_battery_data_t bd; broker_battery_read(&bd);

#ifdef CONFIG_PM_ENABLE
        if (vbus_lock) {
            if (bd.power_good && !vbus_lock_held) {
                esp_pm_lock_acquire(vbus_lock);
                vbus_lock_held = true;
                ESP_LOGI(TAG, "BATT_TEST: VBUS present -- PM light-sleep SUSPENDED (serial alive)");
            } else if (!bd.power_good && vbus_lock_held) {
                esp_pm_lock_release(vbus_lock);
                vbus_lock_held = false;
                ESP_LOGI(TAG, "BATT_TEST: VBUS removed -- PM light-sleep RE-ENGAGED (serial may drop)");
            }
        }
#endif

        char iso[32]; rtc_iso_now(iso, sizeof(iso));

        uint32_t heap_free_kb = esp_get_free_heap_size() / 1024;
        rtc_cpu_freq_config_t cpu_cfg;
        rtc_clk_cpu_freq_get_config(&cpu_cfg);
        uint32_t cpu_mhz = cpu_cfg.freq_mhz;
        uint32_t sensors_on = 0;
        if (broker_imu_get_enabled())     sensors_on |= (1 << 0);
        if (broker_mag_get_enabled())     sensors_on |= (1 << 1);
        if (broker_env_get_enabled())     sensors_on |= (1 << 2);
        if (broker_light_get_enabled())   sensors_on |= (1 << 3);
        if (broker_hr_get_enabled())      sensors_on |= (1 << 4);
        if (broker_skin_get_enabled())    sensors_on |= (1 << 5);
        if (broker_battery_get_enabled()) sensors_on |= (1 << 6);
        if (broker_rtc_get_enabled())     sensors_on |= (1 << 7);

        uint32_t vbat_adc_mv = vbat_adc_read_mv();

        if (batt_path_ok) {
            if (sdcard_mount() == ESP_OK) {
                FILE *f = fopen(batt_path, "a");
                if (f) {
                    fprintf(f, "%lu,%s,%.2f,%.0f,%u,%u,%lu,%lu,%lu,0x%02lX,%u\n",
                            (unsigned long)now, iso, t_soc,
                            bd.voltage * 1000.0f, (unsigned)bd.percentage,
                            (unsigned)(bd.charging ? 1 : 0),
                            (unsigned long)vbat_adc_mv,
                            (unsigned long)heap_free_kb,
                            (unsigned long)cpu_mhz,
                            (unsigned long)sensors_on,
                            (unsigned)idle_pct);
                    fclose(f);
                } else {
                    ESP_LOGW(TAG, "BATT_TEST: append fopen failed: %s", strerror(errno));
                }
                (void)sdcard_unmount();
            } else {
                ESP_LOGW(TAG, "BATT_TEST: sdcard_mount failed -- row #%lu lost",
                         (unsigned long)sample_idx);
            }
        }

        ESP_LOGI(TAG, "BATT_TEST #%lu  %s  soc=%.1fC  vbat=%lumV%s  heap=%luKB  cpu=%luMHz  idle=%u%%  sens=0x%02lX",
                 (unsigned long)sample_idx, iso, t_soc,
                 (unsigned long)vbat_adc_mv,
                 bd.charging ? " CHG" : "",
                 (unsigned long)heap_free_kb,
                 (unsigned long)cpu_mhz,
                 (unsigned)idle_pct,
                 (unsigned long)sensors_on);

        vTaskDelay(pdMS_TO_TICKS(LED_BLIP_MS));
        ws2812_set_color(0, 0, 0);

        sample_idx++;
        next_sample_ms += PERIOD_MS;
    }
}
