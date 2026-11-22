/**
 * @file fc_flight.c
 * @brief FCM_FLIGHT recording mode (Stage 38, revised 2026-10-04).
 *
 * Design (revised after Ivan feedback):
 *   - Started/stopped by the RECORD button on the flight tile (or the
 *     `FLIGHT START` / `FLIGHT STOP` CLI verbs). NOT entered from the
 *     physical encoder-cycle mode click -- Flight shares env's green
 *     RGB slot, so cycling to it from the colour-coded hw button is
 *     ambiguous. The mode-cycle dispatch for FCM_FLIGHT is a no-op.
 *   - Recording runs as a background task spawned on start, self-
 *     deleted on stop. The user can swipe to other tiles or let the
 *     display sleep (double-tap body / btn) while the task keeps
 *     writing CSV rows.
 *   - RGB LED is NOT animated during flight. Tile is the status
 *     surface; the LED stays owned by rgb_policy. Discrete by request.
 *
 * CSV format v1 per docs/build_info/Mk2_intel/CSV_Standard.md. First
 * writer in the tree born under the standard.
 *
 * Durability: fflush + fsync every N_ROWS_PER_FSYNC rows (~96 ms worst
 * case loss at ~125 Hz). PSRAM ring buffer is a later port-era move.
 *
 * Freshness: one row per sample tick (~8 ms = ~125 Hz, nearest the
 * 100 Hz target given the LSM driver's supported ODR of 120 Hz). LIS,
 * BME, GPS carry last-known values with a `_fresh` boolean so post-
 * processing can filter.
 */

#include "fc_internal.h"
#include "firmware_version.h"
#include "fc_flight.h"

#include <string.h>
#include <errno.h>
#include <math.h>
#include <unistd.h>   // fsync(), fileno()

#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "data_broker.h"
#include "haptic.h"
#include "sdcard.h"
#include "pcf85063_cmd.h"
#include "max_m10s.h"

static const char *TAG = "FLIGHT";

#define ROW_PERIOD_MS           8     // ~125 Hz row cadence
#define N_ROWS_PER_FSYNC       12     // ~96 ms worst-case loss window
#define FLIGHT_TASK_STACK_B  4096
#define FLIGHT_TASK_PRIO        1     // same priority tier as blackbox/gps_log
#define FLIGHT_TASK_CORE        0     // sensor-side core, keeps LVGL Core 1 live
#define MS2_TO_MG               (1000.0f / 9.80665f)
#define UT_TO_MG                10.0f

// -- Shared state (readable by the flight tile) -------------------------------
static fc_flight_state_t s_fs = {
    .active        = false,
    .start_ms      = 0,
    .elapsed_s     = 0,
    .sample_count  = 0,
    .write_errors  = 0,
    .sd_ok         = false,
    .sd_free_mib   = 0,
    .gps_fix       = 0,
};

static volatile bool s_stop_request = false;
static FILE         *s_file         = NULL;

const fc_flight_state_t *fc_flight_get_state(void) {
    return &s_fs;
}

static void device_id_short(char *out, size_t n) {
    uint8_t mac[6] = {0};
    (void)esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(out, n, "%02x%02x%02x%02x", mac[2], mac[3], mac[4], mac[5]);
}

static FILE *flight_csv_open(void) {
    ensure_sd();
    if (!s_sd_ready) return NULL;

    char path[128];
    fc_dated_path("flight", "flight", path, sizeof(path));
    FILE *f = fopen(path, "w");
    if (!f) {
        ESP_LOGE(TAG, "fopen %s failed: %s (errno=%d)",
                 path, strerror(errno), errno);
        return NULL;
    }

    char iso[32]; rtc_iso_now(iso, sizeof(iso));
    broker_rtc_data_t rtc; broker_rtc_read(&rtc);
    char id[16]; device_id_short(id, sizeof(id));

    // CSV_Standard v1 header block.
    fprintf(f, "# device_hw = %s\n",            KOMPIC_HW_VERSION);
    fprintf(f, "# device_fw = %s\n",            KOMPIC_FW_VERSION);
    fprintf(f, "# device_id = %s\n",            id);
    fprintf(f, "# record_mode = flight\n");
    fprintf(f, "# record_start_utc = %s\n",     iso);
    fprintf(f, "# record_start_tz_offset_h = +0\n");
    fprintf(f, "# rtc_valid_at_start = %d\n",   rtc.valid ? 1 : 0);
    fprintf(f, "# sample_rate_hz = %d\n",       (int)(1000 / ROW_PERIOD_MS));
    fprintf(f, "# notes = sparse channels carry last-known value + _fresh flag\n");
    fprintf(f, "# columns = ts_ms, ax_mg, ay_mg, az_mg, gx_mdps, gy_mdps, gz_mdps, "
               "mx_mG, my_mG, mz_mG, temp_c, p_hpa, h_pct, gas_ohm, "
               "mag_fresh, env_fresh, "
               "gps_lat_deg, gps_lon_deg, gps_alt_m, gps_fix\n");
    fprintf(f, "# units   = ms, mg, mg, mg, mdps, mdps, mdps, "
               "mG, mG, mG, degC, hPa, pct, ohm, "
               "bool, bool, deg, deg, m, enum\n");
    fprintf(f, "ts_ms,ax_mg,ay_mg,az_mg,gx_mdps,gy_mdps,gz_mdps,"
               "mx_mG,my_mG,mz_mG,temp_c,p_hpa,h_pct,gas_ohm,"
               "mag_fresh,env_fresh,"
               "gps_lat_deg,gps_lon_deg,gps_alt_m,gps_fix\n");

    ESP_LOGI(TAG, "CSV %s opened", path);
    return f;
}

static void write_row(FILE *f,
                      uint32_t ts_ms,
                      const broker_imu_data_t *im,
                      const broker_mag_data_t *mg,  bool mag_fresh,
                      const broker_env_data_t *env, bool env_fresh,
                      const broker_gps_data_t *gps) {
    float ax_mg   = im->accel_x * MS2_TO_MG;
    float ay_mg   = im->accel_y * MS2_TO_MG;
    float az_mg   = im->accel_z * MS2_TO_MG;
    float gx_mdps = im->gyro_x  * 1000.0f;
    float gy_mdps = im->gyro_y  * 1000.0f;
    float gz_mdps = im->gyro_z  * 1000.0f;
    float mx_mG   = mg->x_ut    * UT_TO_MG;
    float my_mG   = mg->y_ut    * UT_TO_MG;
    float mz_mG   = mg->z_ut    * UT_TO_MG;

    fprintf(f,
        "%lu,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,"
        "%.1f,%.1f,%.1f,%.3f,%.3f,%.3f,%.0f,"
        "%d,%d,",
        (unsigned long)ts_ms,
        ax_mg, ay_mg, az_mg, gx_mdps, gy_mdps, gz_mdps,
        mx_mG, my_mG, mz_mG,
        env->temperature_c, env->pressure_hpa, env->humidity_pct, env->gas_resistance_ohm,
        mag_fresh ? 1 : 0, env_fresh ? 1 : 0);

    // CSV enum: 0=none, 2=2D, 3=3D (NMEA convention). Broker's internal
    // GPS_FIX_* enum ordinals are 0/1/2; translate for the file.
    int csv_fix = (gps->fix == GPS_FIX_3D) ? 3
                : (gps->fix == GPS_FIX_2D) ? 2 : 0;
    if (csv_fix != 0) {
        fprintf(f, "%.6f,%.6f,%.2f,%d\n",
                gps->latitude, gps->longitude, gps->altitude_m, csv_fix);
    } else {
        fprintf(f, ",,,0\n");
    }
}

// -- Background flight task ---------------------------------------------------
static void task_flight_fn(void *arg) {
    (void)arg;

    uint32_t start_ms           = millis_u32();
    uint32_t last_mag_update_ms = 0;
    uint32_t last_env_update_ms = 0;
    uint32_t rows_since_fsync   = 0;
    uint32_t next_tile_poll_ms  = start_ms + 200;

    s_fs.start_ms     = start_ms;
    s_fs.sample_count = 0;
    s_fs.write_errors = 0;
    s_fs.sd_ok        = true;
    s_fs.sd_free_mib  = (uint32_t)sdcard_get_free_mib();
    s_fs.gps_fix      = 0;

    broker_imu_data_t im  = {0};
    broker_mag_data_t mg  = {0};
    broker_env_data_t env = {0};
    broker_gps_data_t gps = {0};

    while (!s_stop_request) {
        uint32_t now = millis_u32();

        broker_imu_read(&im);
        broker_mag_read(&mg);
        broker_env_read(&env);
        broker_gps_read(&gps);

        bool mag_fresh = (mg.last_update_ms != last_mag_update_ms);
        bool env_fresh = (env.last_update_ms != last_env_update_ms);
        if (mag_fresh) last_mag_update_ms = mg.last_update_ms;
        if (env_fresh) last_env_update_ms = env.last_update_ms;

        write_row(s_file, now, &im, &mg, mag_fresh, &env, env_fresh, &gps);
        s_fs.sample_count++;
        rows_since_fsync++;

        if (rows_since_fsync >= N_ROWS_PER_FSYNC) {
            rows_since_fsync = 0;
            if (fflush(s_file) != 0 || fsync(fileno(s_file)) != 0) {
                s_fs.write_errors++;
                s_fs.sd_ok = false;
                ESP_LOGW(TAG, "fsync failed (errno=%d) -- long buzz, continuing",
                         errno);
                haptic_play(DRV_LONG_BUZZ);
            } else if (!s_fs.sd_ok) {
                s_fs.sd_ok = true;  // recovered
            }
        }

        if (now >= next_tile_poll_ms) {
            next_tile_poll_ms = now + 500;
            s_fs.elapsed_s   = (now - start_ms) / 1000;
            s_fs.sd_free_mib = (uint32_t)sdcard_get_free_mib();
            s_fs.gps_fix     = (gps.fix == GPS_FIX_3D) ? 3
                              : (gps.fix == GPS_FIX_2D) ? 2 : 0;
        }

        vTaskDelay(pdMS_TO_TICKS(ROW_PERIOD_MS));
    }

    fflush(s_file);
    fsync(fileno(s_file));
    fclose(s_file);
    s_file = NULL;

    // Double buzz on stop.
    haptic_play(DRV_MEDIUM_CLICK);
    vTaskDelay(pdMS_TO_TICKS(80));
    haptic_play(DRV_MEDIUM_CLICK);

    ESP_LOGI(TAG, "flight closed: %lu samples, %lu write errors",
             (unsigned long)s_fs.sample_count,
             (unsigned long)s_fs.write_errors);

    // Leave sensor brokers enabled so other tiles see live data without
    // the user having to re-toggle them. The user manages per-sensor
    // enables from each tile.

    s_fs.active = false;
    vTaskDelete(NULL);
}

// -- Public start / stop surface ---------------------------------------------
int fc_flight_cmd_start(void) {
    if (s_fs.active) {
        ESP_LOGW(TAG, "start requested but already active");
        return -1;
    }

    // Wake the sensors we read. Leaves them on after stop (see task_flight_fn).
    broker_imu_set_enabled(true);
    broker_mag_set_enabled(true);
    broker_env_set_enabled(true);

    // Short settle so first few rows aren't zeros.
    vTaskDelay(pdMS_TO_TICKS(100));

    s_file = flight_csv_open();
    if (!s_file) {
        ESP_LOGE(TAG, "flight CSV open failed -- start aborted");
        haptic_play(DRV_LONG_BUZZ);
        return -2;
    }

    s_stop_request = false;
    s_fs.active    = true;    // set BEFORE spawning so tile sees it immediately

    BaseType_t ok = xTaskCreatePinnedToCore(task_flight_fn, "task_flight",
                                            FLIGHT_TASK_STACK_B, NULL,
                                            FLIGHT_TASK_PRIO, NULL,
                                            FLIGHT_TASK_CORE);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreatePinnedToCore failed");
        fclose(s_file);
        s_file      = NULL;
        s_fs.active = false;
        return -3;
    }

    haptic_play(DRV_STRONG_CLICK);
    ESP_LOGI(TAG, "flight recording started");
    return 0;
}

int fc_flight_cmd_stop(void) {
    if (!s_fs.active) return -1;
    s_stop_request = true;
    ESP_LOGI(TAG, "flight stop requested");
    return 0;
}
