/**
 * @file fc_gps_log.c
 * @brief Stage 36 -- background 1 Hz GPS CSV logger.
 *
 * Runs alongside BLACKBOX (independent NVS flag `gps_log`). Writes to
 * /sd/data/gps/gps_<datetime>.csv, open-append-close per row for
 * durability ([[feedback_sd_write_durable]]). Every row snapshots the
 * SNR summary, MON-RF (noise/AGC/antenna), fix state, per-class UBX
 * counters, and 1PPS count -- so a walking bench with the laptop off
 * still produces post-hoc-analysable data.
 */

#include "fc_internal.h"
#include "firmware_version.h"

#include <string.h>
#include <errno.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/task.h"

#include "nvs_cfg.h"
#include "data_broker.h"
#include "max_m10s.h"

static const char *TAG = "FC_GPSLOG";

#define GPS_LOG_CADENCE_MS  1000U

void task_gps_log_fn(void *arg)
{
    (void)arg;

    if (!nvs_cfg_sys_get_gps_log()) {
        ESP_LOGI(TAG, "GPS_LOG: disabled by NVS flag -- task exiting");
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "GPS_LOG: task started on Core %d", xPortGetCoreID());

    for (int i = 0; i < 50 && !s_sd_ready; i++) {
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    if (!s_sd_ready) {
        ESP_LOGE(TAG, "GPS_LOG: SD never mounted -- task exiting");
        vTaskDelete(NULL);
        return;
    }
    try_mkdir("/sd/data/gps");

    char path[96];
    fc_dated_path("gps", "gps", path, sizeof(path));
    {
        FILE *f = fopen(path, "w");
        if (!f) {
            ESP_LOGE(TAG, "GPS_LOG: fopen %s failed: %s", path, strerror(errno));
            vTaskDelete(NULL);
            return;
        }
        char now[32]; rtc_iso_now(now, sizeof(now));
        fprintf(f, "# kompic mk1 %s fw=%s hw=%s mode=gps_log rtc_start=%s cadence_ms=%u\n",
                KOMPIC_HW_VERSION, KOMPIC_FW_VERSION, KOMPIC_HW_VERSION,
                now, (unsigned)GPS_LOG_CADENCE_MS);
        fprintf(f, "# ant_stat: 0=INIT 1=DONTKNOW 2=OK 3=SHORT 4=OPEN\n");
        fprintf(f, "# ant_pwr : 0=OFF 1=ON 2=DONTKNOW\n");
        fprintf(f, "# fix     : 0=NONE 1=2D 2=3D\n");
        fprintf(f, "# CN0 in dBHz; noise_ms + agc_cnt raw M10 units; agc_pct = agc_cnt*100/8191\n");
        fprintf(f, "t_ms,iso_utc,"
                   "sats_snr,sats_view,max_cn0,top4_avg,"
                   "ant_stat,ant_pwr,noise_ms,agc_cnt,agc_pct,"
                   "fix,sats_use,hdop,lat,lon,alt_m,speed_kmh,course_deg,"
                   "mon_ver_ct,mon_rf_ct,timeutc_ct,ack_ack,ack_nak,pps_ct\n");
        fclose(f);
        ESP_LOGI(TAG, "GPS_LOG: logging to %s (1 Hz, open-append-close per row)", path);
    }

    uint32_t next_ms = millis_u32();
    uint32_t idx     = 0;

    for (;;) {
        uint32_t now = millis_u32();
        if ((int32_t)(now - next_ms) < 0) {
            uint32_t wait = next_ms - now;
            if (wait > 500) wait = 500;
            vTaskDelay(pdMS_TO_TICKS(wait));
            continue;
        }

        char iso[32]; rtc_iso_now(iso, sizeof(iso));

        max_m10s_snr_summary_t snr = {0};
        max_m10s_monrf_t       rf  = {0};
        max_m10s_ubx_counters_t ct = {0};
        broker_gps_data_t      gd  = {0};
        max_m10s_get_snr_summary(&snr);
        max_m10s_get_monrf(&rf);
        max_m10s_get_ubx_counters(&ct);
        broker_gps_read(&gd);

        int agc_pct = (int)(((uint32_t)rf.agc_cnt * 100U) / 8191U);
        uint32_t pps_ct = max_m10s_get_pps_count();

        FILE *f = fopen(path, "a");
        if (f) {
            fprintf(f, "%lu,%s,"
                       "%u,%u,%u,%u,"
                       "%u,%u,%u,%u,%d,"
                       "%d,%u,%.2f,%.7f,%.7f,%.1f,%.2f,%.1f,"
                       "%lu,%lu,%lu,%lu,%lu,%lu\n",
                    (unsigned long)now, iso,
                    (unsigned)snr.sats_with_snr, (unsigned)snr.sats_in_view,
                    (unsigned)snr.max_cn0,       (unsigned)snr.top4_avg_cn0,
                    (unsigned)rf.ant_status,     (unsigned)rf.ant_power,
                    (unsigned)rf.noise_per_ms,   (unsigned)rf.agc_cnt, agc_pct,
                    (int)gd.fix, (unsigned)gd.sats_in_use, (double)gd.hdop,
                    gd.latitude, gd.longitude, (double)gd.altitude_m,
                    (double)gd.speed_kmh, (double)gd.course_deg,
                    (unsigned long)ct.mon_ver, (unsigned long)ct.mon_rf,
                    (unsigned long)ct.nav_timeutc,
                    (unsigned long)ct.ack_ack, (unsigned long)ct.ack_nak,
                    (unsigned long)pps_ct);
            fclose(f);
        } else {
            ESP_LOGW(TAG, "GPS_LOG: append fopen failed: %s (SD unmounted?)",
                     strerror(errno));
        }

        if ((idx % 60) == 0) {
            ESP_LOGI(TAG, "GPS_LOG #%lu  %s  sats=%u  cn0max=%u  fix=%d",
                     (unsigned long)idx, iso,
                     (unsigned)snr.sats_with_snr, (unsigned)snr.max_cn0,
                     (int)gd.fix);
        }

        idx++;
        next_ms += GPS_LOG_CADENCE_MS;
    }
}
