/**
 * @file fc_cli.c
 * @brief USB-Serial-JTAG CLI: RTC, NVS, BATT_TEST, BLACKBOX, WHOAMI,
 *        TEMP_DUMP, REC_AUDIO, SHIPMODE, filesystem, RGB poke.
 *
 * Split out of field_capture.c in the Stage 12 refactor. Runs on its own task
 * (task_rtc_cli_fn) created by boot_tasks.c. Line assembly reads directly from
 * the USB-Serial-JTAG driver; command dispatch is case-insensitive prefix.
 */

#include "fc_internal.h"
#include "firmware_version.h"

#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/stat.h>
#include <dirent.h>

#include "driver/i2c.h"
#include "driver/usb_serial_jtag.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "soc/rtc.h"

#include "data_broker.h"
#include "ws2812.h"
#include "sdcard.h"
#include "mic_pdm.h"
#include "haptic.h"
#include "drv2605_cmd.h"
#include "bq25619.h"
#include "bq25619_cmd.h"
#include "max17048.h"
#include "veml6030_cmd.h"
#include "pcf85063_cmd.h"
#include "encoder_cmd.h"
#include "ws2812_cmd.h"
#include "flashlight.h"
#include "lvgl_ui_screenshot.h"
#include "pcf85063.h"
#include "nvs_cfg.h"
#include "lsm6dsv16x.h"

static const char *TAG = "FC_CLI";

// Forward decl: boot_pm_dump_locks lives in components/boot_logic/boot_pm.c.
// Not adding boot_logic to field_capture's REQUIRES (would form a cycle).
extern void boot_pm_dump_locks(void);

// Boot-seq accessor lives in field_capture.c (public getter).
extern uint32_t field_capture_get_boot_seq(void);

// ── Parsing helpers ─────────────────────────────────────────────────────────
static int try_parse_iso(const char *s, int *yr, int *mo, int *da,
                         int *hr, int *mi, int *se) {
    if (sscanf(s, "%d-%d-%dT%d:%d:%d", yr, mo, da, hr, mi, se) == 6) return 1;
    if (sscanf(s, "%d-%d-%d %d:%d:%d", yr, mo, da, hr, mi, se) == 6) return 1;
    return 0;
}

int startswith_ci(const char *s, const char *pfx) {
    while (*pfx) {
        char a = *s++, b = *pfx++;
        if (a >= 'a' && a <= 'z') a -= 32;
        if (b >= 'a' && b <= 'z') b -= 32;
        if (a != b) return 0;
    }
    return 1;
}

static void rtc_cli_print_now(void) {
    broker_rtc_data_t r; broker_rtc_read(&r);
    if (r.valid) {
        printf("[RTC] %04u-%02u-%02uT%02u:%02u:%02u UTC\n",
               (unsigned)r.year, (unsigned)r.month, (unsigned)r.day,
               (unsigned)r.hour, (unsigned)r.minute,(unsigned)r.second);
    } else {
        printf("[RTC] oscstop -- run SET_TIME <YYYY-MM-DDTHH:MM:SS> to seed\n");
    }
}

static uint64_t civil_to_unix(int yr, int mo, int da, int hr, int mi, int se) {
    static const int dpm[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    uint64_t days = 0;
    for (int y = 1970; y < yr; y++) {
        days += ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0) ? 366 : 365;
    }
    for (int m = 0; m < mo - 1; m++) {
        int dm = dpm[m];
        if (m == 1 && ((yr % 4 == 0 && yr % 100 != 0) || yr % 400 == 0)) dm = 29;
        days += dm;
    }
    days += (da - 1);
    return days * 86400ULL + (uint64_t)hr * 3600 + (uint64_t)mi * 60 + (uint64_t)se;
}

// ── NVS dump ────────────────────────────────────────────────────────────────
// Delegates to the authoritative dumper in nvs_cfg.c so this stays in sync
// with new NVS fields (added: lvgl_force, auto_shdn, log_level, pc_sync
// after the local copy fell behind pre-Stage-30).
static void rtc_cli_dump_nvs(void) {
    nvs_cfg_dump(I2C_NUM_0);
}

// ── HELP ────────────────────────────────────────────────────────────────────
// Verb table: kept unsorted here (add new verbs anywhere) -- the print
// routine sorts alphabetically before emitting so the on-screen list is
// always in-order. Ivan asked for this on 2026-09-10 as the flat list
// crossed the "can't scan" threshold.
//
// Format of each line: "VERB [args]        one-line description"
// Two-space padding on the left is added at print time.
static const char *k_help_lines[] = {
    "AUTOSHDN [<min>|OFF]             auto-shutdown minutes (0/OFF = perma-on, default 120, reboot to apply)",
    "FLASH [ON|OFF|<0..100>]          flashlight LED (bypass encoder; no arg = show state)",
    "NVS                              dump every persisted NVS record (on-demand)",
    "BATT_TEST [ON|OFF]               enter battery-test mode on next boot (no arg = state)",
    "BLACKBOX [ON|OFF]                background telemetry logger (reboot to start/stop)",
    "BLACKBOX_CADENCE <s>             sample cadence in seconds (default 10, range 1..3600)",
    "BQ [EN|BOOST [ON|OFF] | SHIPMODE] BQ25619 command surface (no arg = dump)",
    "ENC [RESET]                      encoder status + reg dump (RESET = zero counters)",
    "FS_CAT </sd/path>                dump a file to console",
    "FS_LS [/sd/path]                 list SD directory",
    "DISP [ON|OFF]                    CO5300 panel sleep/wake (batt-test tool; no arg = state)",
    "FUEL                             MAX17048 VERSION + VCELL + fuel % (needs cell attached)",
    "GESTURE                          dump wrist-gesture state + LPF value",
    "GET_TIME [-v]                    read RTC now (-v also dumps NVS + RAM_byte)",
    "GPS_DYNMODEL [PORT|STAT|PED|AUTO|SEA|AIR1|AIR2|AIR4|WRIST|BIKE]  set MAX-M10S dynamic model (no arg = show)",
    "GPS_MONRF [ON]                   dump UBX-MON-RF snapshot (ON = re-issue enable frame first)",
    "GPS_PING                         send UBX-MON-VER poll; check GPS_UBX_STATS for mon_ver++ within a second",
    "GPS_UBX_STATS                    per-class UBX frame receive counters (0 across the board = TX broken)",
    "GPS_SNR                          NMEA GSV-derived signal summary (top-4 CN0, sats in view)",
    "GPS_VIEW [normal|photo|toggle]   switch GPS tile between telemetry + photo layouts (no arg = state)",
    "HAPTIC [EN|PLAY <n>|CAL|SWEEP START|STOP|UI [<n>]]  (no arg = dump)",
    "HELP                             this list",
    "LIGHT [EN|AUTO|BLUE [ON|OFF] | BR [<0..100>]]       (no arg = dump)",
    "LOGLEVEL [OFF|E|W|I|D|V|AUTO]    runtime esp_log level (no arg = show current)",
    "LVGL_FORCE [ON|OFF]              force LVGL up on next boot even without a panel (no arg = state)",
    "LVGL_SCREENSHOT                  capture active screen -> /sd/lvgl_fb/*.png",
    "NVS_PRINT [ON|OFF]               toggle boot-time NVS printout (no arg = dump)",
    "PM_DUMP                          dump PM lock inventory now",
    "REBOOT                           esp_restart() -- clean SW reset",
    "REC_AUDIO [ON|OFF]               5 s voice annotation before ENV/MOTION/SKIN (no arg = state)",
    "RGB <r> <g> <b> | AUTO           bench-poke WS2812 / release override (no arg = dump)",
    "RTC                              PCF85063A status + register dump",
    "RTC_DUMP                         hex dump of all 18 PCF85063A registers",
    "SET_TIME YYYY-MM-DDTHH:MM:SS     write UTC + persist to NVS + PCF RAM_byte",
    "SHIPMODE                         drop BATFET now (escape when button stuck)",
    "STATUS                           one-shot state dump (uptime, sensors, batt, heap)",
    "TEMP_DUMP                        read every onboard temp source (waits for stable, non-zero)",
    "TILE [list | <n> | <name>]       jump tileview to a tile (bench-testable under LVGL_FORCE)",
    "TOUCH                            dump CST9217 touch state (last x/y, event count, pressed)",
    "WHOAMI                           I2C sensor identification + hw_alive status",
};

static int help_line_cmp(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

static void rtc_cli_print_help(void) {
    const size_t n = sizeof(k_help_lines) / sizeof(k_help_lines[0]);
    const char *sorted[sizeof(k_help_lines) / sizeof(k_help_lines[0])];
    memcpy(sorted, k_help_lines, sizeof(sorted));
    qsort(sorted, n, sizeof(sorted[0]), help_line_cmp);
    printf("  Commands:\n");
    for (size_t i = 0; i < n; i++) {
        printf("    %s\n", sorted[i]);
    }
}

// ── STATUS ──────────────────────────────────────────────────────────────────
static void rtc_cli_dump_status(void) {
    uint32_t up_s = millis_u32() / 1000U;
    uint32_t h = up_s / 3600; uint32_t m = (up_s % 3600) / 60; uint32_t s = up_s % 60;

    rtc_cpu_freq_config_t cfg;
    rtc_clk_cpu_freq_get_config(&cfg);
    uint32_t heap_kb = esp_get_free_heap_size() / 1024;
    uint32_t min_kb  = esp_get_minimum_free_heap_size() / 1024;

    broker_battery_data_t bat; broker_battery_read(&bat);

    esp_ts_ensure_init();
    float t_esp = esp_ts_read_c();

    uint32_t sens_on = 0;
    if (broker_imu_get_enabled())     sens_on |= (1 << 0);
    if (broker_mag_get_enabled())     sens_on |= (1 << 1);
    if (broker_env_get_enabled())     sens_on |= (1 << 2);
    if (broker_light_get_enabled())   sens_on |= (1 << 3);
    if (broker_hr_get_enabled())      sens_on |= (1 << 4);
    if (broker_skin_get_enabled())    sens_on |= (1 << 5);
    if (broker_battery_get_enabled()) sens_on |= (1 << 6);
    if (broker_rtc_get_enabled())     sens_on |= (1 << 7);

    printf("[STATUS]\n");
    printf("  fw           = %s   hw = %s\n", KOMPIC_FW_VERSION, KOMPIC_HW_VERSION);
    printf("  uptime       = %luh %02lum %02lus   boot_seq = %lu\n",
           (unsigned long)h, (unsigned long)m, (unsigned long)s,
           (unsigned long)s_boot_seq);
    printf("  fcm_mode     = %s   state = %s\n",
           MODE_INFO[s_mode].name,
           (s_state == 0) ? "STANDBY" :
           (s_state == 1) ? "FL_ON"   :
           (s_state == 2) ? "RECORDING" : "ALARM");
    printf("  sensors_on   = 0x%02lX  (IMU|MAG|ENV|LIGHT|HR|SKIN|BAT|RTC, LSB=IMU)\n",
           (unsigned long)sens_on);
    printf("  cpu_mhz      = %lu   heap = %lu KB (min %lu KB)\n",
           (unsigned long)cfg.freq_mhz, (unsigned long)heap_kb,
           (unsigned long)min_kb);
    printf("  esp_temp     = %.1f C  (ESP32-S3 die)\n", t_esp);
    {
        uint16_t vcell_mv = 0, soc_pct100 = 0;
        esp_err_t rv = ESP_FAIL, rs = ESP_FAIL;
        if (xSemaphoreTake(g_i2c2_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            rv = max17048_read_vcell_mv(I2C_NUM_1, &vcell_mv);
            rs = max17048_read_soc_pct100(I2C_NUM_1, &soc_pct100);
            xSemaphoreGive(g_i2c2_mutex);
        }
        if (rv == ESP_OK && rs == ESP_OK) {
            printf("  vcell        = %u mV   fuel = %u.%02u %%   charging = %d  pg = %d  fault = 0x%02X\n",
                   vcell_mv, soc_pct100 / 100U, soc_pct100 % 100U,
                   bat.charging ? 1 : 0, bat.power_good ? 1 : 0, bat.fault);
        } else {
            printf("  vcell        = n/a   charging = %d  pg = %d  fault = 0x%02X\n",
                   bat.charging ? 1 : 0, bat.power_good ? 1 : 0, bat.fault);
        }
    }
    // Display probe result -- proves boot_display_init ran even when the boot
    // log dropped its BOOT_DISP lines (persistent USB Serial JTAG dropout).
    {
        extern bool boot_display_is_present(void);
        extern bool boot_display_touch_is_present(void);
        printf("  display      = %s  touch = %s (Stage 21/22)\n",
               boot_display_is_present() ? "PRESENT (CO5300 up)" : "absent (headless)",
               boot_display_touch_is_present() ? "CST9217 up" : "off");
    }
    // LVGL bring-up state (Stage 22 §4.1b). up=real panel, forced=bench-only
    // (flush is a no-op), off=LVGL never came up.
    {
        extern bool   lvgl_ui_display_is_up(void);
        extern bool   lvgl_ui_display_is_forced(void);
        extern size_t lvgl_ui_display_buf_bytes(void);
        if (lvgl_ui_display_is_up()) {
            printf("  lvgl         = %s  buf=%lu KB\n",
                   lvgl_ui_display_is_forced() ? "forced (no panel)" : "up (CO5300)",
                   (unsigned long)(lvgl_ui_display_buf_bytes() / 1024U));
        } else {
            printf("  lvgl         = off\n");
        }
    }
    printf("  NVS: print_boot=%d  batt_test=%d  blackbox=%d  bb_cadence=%u s  rec_audio=%d  lvgl_force=%d\n",
           nvs_cfg_sys_get_print_on_boot() ? 1 : 0,
           nvs_cfg_sys_get_batt_test()     ? 1 : 0,
           nvs_cfg_sys_get_blackbox()      ? 1 : 0,
           (unsigned)nvs_cfg_sys_get_bb_cadence_s(),
           nvs_cfg_sys_get_rec_audio()     ? 1 : 0,
           nvs_cfg_sys_get_lvgl_force_on() ? 1 : 0);
}

// ── WHOAMI helpers ──────────────────────────────────────────────────────────
static uint8_t whoami_read_reg8(i2c_port_t port, uint8_t addr, uint8_t reg,
                                 esp_err_t *out_err) {
    uint8_t val = 0xFF;
    esp_err_t r = ESP_FAIL;
    if (xSemaphoreTake(g_i2c_mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
        i2c_cmd_handle_t cmd = i2c_cmd_link_create();
        i2c_master_start(cmd);
        i2c_master_write_byte(cmd, (addr << 1) | I2C_MASTER_WRITE, true);
        i2c_master_write_byte(cmd, reg, true);
        i2c_master_start(cmd);
        i2c_master_write_byte(cmd, (addr << 1) | I2C_MASTER_READ, true);
        i2c_master_read_byte(cmd, &val, I2C_MASTER_NACK);
        i2c_master_stop(cmd);
        r = i2c_master_cmd_begin(port, cmd, pdMS_TO_TICKS(50));
        i2c_cmd_link_delete(cmd);
        xSemaphoreGive(g_i2c_mutex);
    }
    if (out_err) *out_err = r;
    return val;
}

static uint16_t whoami_read_reg16be(i2c_port_t port, uint8_t addr, uint8_t reg,
                                     esp_err_t *out_err) {
    uint8_t hi = 0xFF, lo = 0xFF;
    esp_err_t r = ESP_FAIL;
    if (xSemaphoreTake(g_i2c_mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
        i2c_cmd_handle_t cmd = i2c_cmd_link_create();
        i2c_master_start(cmd);
        i2c_master_write_byte(cmd, (addr << 1) | I2C_MASTER_WRITE, true);
        i2c_master_write_byte(cmd, reg, true);
        i2c_master_start(cmd);
        i2c_master_write_byte(cmd, (addr << 1) | I2C_MASTER_READ, true);
        i2c_master_read_byte(cmd, &hi, I2C_MASTER_ACK);
        i2c_master_read_byte(cmd, &lo, I2C_MASTER_NACK);
        i2c_master_stop(cmd);
        r = i2c_master_cmd_begin(port, cmd, pdMS_TO_TICKS(50));
        i2c_cmd_link_delete(cmd);
        xSemaphoreGive(g_i2c_mutex);
    }
    if (out_err) *out_err = r;
    return ((uint16_t)hi << 8) | lo;
}

static void rtc_cli_dump_whoami(void) {
    printf("[WHOAMI]\n");
    printf("  bus  addr  chip         who@reg  read     expect  hw_alive  status\n");

    // PCF85063A -- no WHO_AM_I register.
    printf("  I2C0 0x51  %-11s  -        -        -       %d         %s\n",
           "PCF85063A", broker_rtc_hw_alive() ? 1 : 0,
           broker_rtc_hw_alive() ? "OK (no chip-ID reg, hw_alive from I2C probe)" : "FAILED");

    { esp_err_t e; uint8_t got = whoami_read_reg8(I2C_NUM_0, 0x76, 0xD0, &e);
      printf("  I2C0 0x76  %-11s  0xD0     0x%02X     0x61    %d         %s\n",
             "BME688", got, broker_env_hw_alive() ? 1 : 0,
             (e != ESP_OK) ? "I2C_ERR" : (got == 0x61 ? "OK" : "MISMATCH")); }
    { esp_err_t e; uint8_t got = whoami_read_reg8(I2C_NUM_0, 0x6B, 0x0F, &e);
      printf("  I2C0 0x6B  %-11s  0x0F     0x%02X     0x70    %d         %s\n",
             "LSM6DSV16X", got, broker_imu_hw_alive() ? 1 : 0,
             (e != ESP_OK) ? "I2C_ERR" : (got == 0x70 ? "OK" : "MISMATCH")); }
    { esp_err_t e; uint8_t got = whoami_read_reg8(I2C_NUM_0, 0x1C, 0x0F, &e);
      printf("  I2C0 0x1C  %-11s  0x0F     0x%02X     0x3D    %d         %s\n",
             "LIS3MDL", got, broker_mag_hw_alive() ? 1 : 0,
             (e != ESP_OK) ? "I2C_ERR" : (got == 0x3D ? "OK" : "MISMATCH")); }
    { esp_err_t e; uint8_t got = whoami_read_reg8(I2C_NUM_0, 0x57, 0xFF, &e);
      printf("  I2C0 0x57  %-11s  0xFF     0x%02X     0x15    %d         %s\n",
             "MAX30101", got, broker_hr_hw_alive() ? 1 : 0,
             (e != ESP_OK) ? "I2C_ERR" : (got == 0x15 ? "OK" : "MISMATCH")); }
    { esp_err_t e; uint16_t got = whoami_read_reg16be(I2C_NUM_0, 0x48, 0x0F, &e);
      uint16_t low12 = got & 0x0FFF;
      printf("  I2C0 0x48  %-11s  0x0F     0x%04X   0x0117  %d         %s\n",
             "TMP117", got, broker_skin_hw_alive() ? 1 : 0,
             (e != ESP_OK) ? "I2C_ERR" : (low12 == 0x117 ? "OK" : "MISMATCH")); }
    printf("  I2C0 0x10  %-11s  -        -        -       %d         %s\n",
           "VEML6030", broker_light_hw_alive() ? 1 : 0,
           broker_light_hw_alive() ? "OK (no chip-ID reg)" : "FAILED");
    { esp_err_t e; uint8_t got = whoami_read_reg8(I2C_NUM_1, 0x6A, 0x0A, &e);
      printf("  I2C1 0x6A  %-11s  0x0A     0x%02X     0x80    %d         %s\n",
             "BQ25619", got, broker_battery_hw_alive() ? 1 : 0,
             (e != ESP_OK) ? "I2C_ERR" : (got == 0x80 ? "OK" : "MISMATCH")); }
    { esp_err_t e; uint8_t got = whoami_read_reg8(I2C_NUM_1, 0x5A, 0x00, &e);
      uint8_t dev_id = got & 0xE0;
      printf("  I2C1 0x5A  %-11s  0x00     0x%02X     0xE0    %d         %s\n",
             "DRV2605", got, broker_haptic_get_status() != SENSOR_OFFLINE,
             (e != ESP_OK) ? "I2C_ERR" :
             (dev_id == 0xE0 ? "OK (DEV_ID match; low bits = diagnostic)" : "MISMATCH")); }
    // MAX17048 is 16-bit big-endian at reg 0x08; family mask = 0x001_
    // where the low nibble is the IC production revision (per datasheet).
    { uint16_t ver = 0;
      esp_err_t e = ESP_FAIL;
      if (xSemaphoreTake(g_i2c2_mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
          e = max17048_read16(I2C_NUM_1, MAX17048_REG_VERSION, &ver);
          xSemaphoreGive(g_i2c2_mutex);
      }
      printf("  I2C1 0x36  %-11s  0x08     0x%04X   0x001_  -         %s\n",
             "MAX17048", ver,
             (e != ESP_OK) ? "I2C_ERR (no cell?)" :
             ((ver & 0xFFF0) == 0x0010 ? "OK" : "MISMATCH")); }

    printf("  --\n");
    if (sdcard_is_mounted()) {
        printf("  SD card               mounted=1  cap=%ld MiB  free=%ld MiB\n",
               (long)sdcard_get_capacity_mib(), (long)sdcard_get_free_mib());
    } else {
        printf("  SD card               mounted=0\n");
    }
    printf("  Mic (PDM)             running=%d\n", mic_pdm_is_running() ? 1 : 0);
}

// ── TEMP_DUMP ───────────────────────────────────────────────────────────────
static void rtc_cli_dump_temps(void) {
    printf("[TEMP_DUMP] waking sensors, polling for stable readings...\n");

    // Snapshot enabled state so we only park what we woke.
    bool had_env  = broker_env_get_enabled();
    bool had_imu  = broker_imu_get_enabled();
    bool had_skin = broker_skin_get_enabled();

    if (!had_env)  broker_env_set_enabled(true);
    if (!had_imu)  broker_imu_set_enabled(true);
    if (!had_skin) broker_skin_set_enabled(true);

    esp_ts_ensure_init();

    const int   TICK_MS        = 500;
    const int   TIMEOUT_MS     = 15000;
    const float STABLE_DELTA_C = 0.5f;
    const int   STABLE_HITS    = 2;

    float t_tmp = 0, t_bme = 0, t_lsm = 0, t_max = 0, t_esp = 0;
    float p_tmp = 999, p_bme = 999, p_lsm = 999, p_max = 999, p_esp = 999;
    int   hits = 0;
    int   elapsed_ms = 0;
    bool  timed_out = true;

    vTaskDelay(pdMS_TO_TICKS(300));

    while (elapsed_ms <= TIMEOUT_MS) {
        broker_env_data_t  e; broker_env_read(&e);
        broker_skin_data_t s; broker_skin_read(&s);

        t_tmp = s.skin_temp_c;
        t_bme = e.temperature_c;
        t_lsm = read_lsm_die_temp();
        t_max = read_max_die_temp();
        t_esp = esp_ts_read_c();

        #define VALID_C(v) ((v) > -40.0f && (v) < 120.0f && (v) != 0.0f)
        bool all_valid = VALID_C(t_tmp) && VALID_C(t_bme) &&
                         VALID_C(t_lsm) && VALID_C(t_max) && VALID_C(t_esp);
        #undef VALID_C

        bool all_stable =
            fabsf(t_tmp - p_tmp) < STABLE_DELTA_C &&
            fabsf(t_bme - p_bme) < STABLE_DELTA_C &&
            fabsf(t_lsm - p_lsm) < STABLE_DELTA_C &&
            fabsf(t_max - p_max) < STABLE_DELTA_C &&
            fabsf(t_esp - p_esp) < STABLE_DELTA_C;

        if (all_valid && all_stable) {
            if (++hits >= STABLE_HITS) { timed_out = false; break; }
        } else {
            hits = 0;
        }

        p_tmp = t_tmp; p_bme = t_bme; p_lsm = t_lsm;
        p_max = t_max; p_esp = t_esp;

        vTaskDelay(pdMS_TO_TICKS(TICK_MS));
        elapsed_ms += TICK_MS;
    }

    if (timed_out) {
        printf("[TEMP_DUMP] TIMEOUT after %d ms -- printing last read anyway:\n",
               TIMEOUT_MS);
    } else {
        printf("[TEMP_DUMP] settled in %d ms\n", elapsed_ms);
    }
    printf("  skin (TMP117)      = %6.2f C\n", t_tmp);
    printf("  air  (BME688)      = %6.2f C\n", t_bme);
    printf("  imu  (LSM6DSV16X)  = %6.2f C\n", t_lsm);
    printf("  ppg  (MAX30101)    = %6.2f C\n", t_max);
    printf("  esp  (ESP32-S3)    = %6.2f C\n", t_esp);
    {
        // BQ25619 has no numeric TS ADC -- it only reports the JEITA zone.
        broker_battery_data_t bd_now; broker_battery_read(&bd_now);
        printf("  bq   (BQ25619 TS)  = %s (JEITA zone, no numeric °C on this chip)\n",
               bq25619_ntc_status_str(bd_now.fault));
    }

    if (!had_env)  broker_env_set_enabled(false);
    if (!had_imu)  broker_imu_set_enabled(false);
    if (!had_skin) broker_skin_set_enabled(false);
}

// ── Filesystem helpers ──────────────────────────────────────────────────────
static void rtc_cli_fs_ls(const char *path) {
    if (!path || !*path) path = "/sd";
    DIR *d = opendir(path);
    if (!d) {
        printf("[FS_LS] opendir(%s) failed: %s\n", path, strerror(errno));
        return;
    }
    printf("[FS_LS] %s\n", path);
    struct dirent *ent;
    int count = 0;
    while ((ent = readdir(d))) {
        char full[400];
        (void)snprintf(full, sizeof(full), "%s/%s", path, ent->d_name);
        struct stat st;
        if (stat(full, &st) == 0) {
            char kind = S_ISDIR(st.st_mode) ? 'D' : 'F';
            printf("  [%c]  %-30s  %10lld B\n",
                   kind, ent->d_name, (long long)st.st_size);
        } else {
            printf("  [?]  %-30s  (stat failed)\n", ent->d_name);
        }
        count++;
    }
    closedir(d);
    printf("[FS_LS] %d entries\n", count);
}

static void rtc_cli_fs_cat(const char *path) {
    if (!path || !*path) {
        printf("[FS_CAT] usage: FS_CAT /sd/path/to/file\n");
        return;
    }
    FILE *f = fopen(path, "r");
    if (!f) {
        printf("[FS_CAT] fopen(%s) failed: %s\n", path, strerror(errno));
        return;
    }
    printf("[FS_CAT] %s\n", path);
    char buf[256];
    size_t n, total = 0;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        fwrite(buf, 1, n, stdout);
        total += n;
        if (total >= 32 * 1024) {
            printf("\n[FS_CAT] ...truncated at 32 KB\n");
            break;
        }
    }
    fclose(f);
    printf("\n[FS_CAT] %zu bytes\n", total);
}

// ── Dispatcher ──────────────────────────────────────────────────────────────
static void rtc_cli_handle_line(char *line, int64_t t_recv_us) {
    size_t n = strlen(line);
    while (n > 0 && (line[n-1] == ' ' || line[n-1] == '\t' ||
                     line[n-1] == '\r' || line[n-1] == '\n')) {
        line[--n] = 0;
    }
    if (n == 0) return;

    // Dispatch to sync handler when active or when SYNC_START arrives.
    if (fc_sync_is_active() || startswith_ci(line, "SYNC_START")) {
        fc_sync_handle_line(line, t_recv_us);
        return;
    }

    if (startswith_ci(line, "GET_TIME")) {
        rtc_cli_print_now();
        pcf85063_ram_byte_write(I2C_NUM_0, PCF85063_RAM_CMD_GET_TIME);
        const char *arg = line + 8;
        while (*arg == ' ' || *arg == '\t') arg++;
        if (startswith_ci(arg, "-V") || startswith_ci(arg, "VERBOSE")) {
            rtc_cli_dump_nvs();
        }
        return;
    }
    if (startswith_ci(line, "SET_TIME")) {
        const char *arg = line + 8;
        while (*arg == ' ' || *arg == '\t') arg++;
        int yr, mo, da, hr, mi, se;
        if (!try_parse_iso(arg, &yr, &mo, &da, &hr, &mi, &se)) {
            printf("[RTC] SET_TIME parse fail. Format: SET_TIME YYYY-MM-DDTHH:MM:SS\n");
            return;
        }
        if (yr < 2000 || yr > 2099 || mo < 1 || mo > 12 || da < 1 || da > 31 ||
            hr > 23 || mi > 59 || se > 59) {
            printf("[RTC] SET_TIME out-of-range\n");
            return;
        }
        if (!broker_rtc_hw_alive()) {
            printf("[RTC] PCF85063A not alive -- cannot set\n");
            return;
        }
        esp_err_t r = pcf85063_cmd_sync_utc((uint8_t)hr, (uint8_t)mi, (uint8_t)se,
                                            (uint8_t)da, (uint8_t)mo, (uint16_t)yr);
        if (r == ESP_OK) {
            printf("[RTC] SET_TIME OK -> ");
            (void)pcf85063_ram_byte_write(I2C_NUM_0, PCF85063_RAM_CMD_SET_TIME);
            uint64_t wall_ts = civil_to_unix(yr, mo, da, hr, mi, se);
            uint64_t wr_ms   = (uint64_t)(esp_timer_get_time() / 1000LL);
            esp_err_t save   = nvs_cfg_rtc_save(wall_ts, wr_ms,
                                                 field_capture_get_boot_seq(),
                                                 line);
            if (save != ESP_OK) {
                printf("[NVS] SET_TIME save failed: %s\n", esp_err_to_name(save));
            }
            vTaskDelay(pdMS_TO_TICKS(1100));
            rtc_cli_print_now();
            printf("[NVS] persisted: wall_ts=%llu wr_ms=%llu boot_seq=%lu\n",
                   (unsigned long long)wall_ts, (unsigned long long)wr_ms,
                   (unsigned long)field_capture_get_boot_seq());
        } else {
            printf("[RTC] SET_TIME write failed: %s\n", esp_err_to_name(r));
        }
        return;
    }
    if (startswith_ci(line, "REBOOT")) {
        printf("[REBOOT] esp_restart() in 200 ms\n");
        fflush(stdout);
        vTaskDelay(pdMS_TO_TICKS(200));
        esp_restart();
        return;
    }
    if (startswith_ci(line, "HELP"))      { rtc_cli_print_help();  return; }
    if (startswith_ci(line, "STATUS"))    { rtc_cli_dump_status(); return; }
    if (startswith_ci(line, "WHOAMI"))    { rtc_cli_dump_whoami(); return; }
    if (startswith_ci(line, "TEMP_DUMP")) { rtc_cli_dump_temps();  return; }
    if (startswith_ci(line, "PM_DUMP"))   { boot_pm_dump_locks();  return; }
    if (startswith_ci(line, "RTC_DUMP"))  { pcf85063_cmd_dump(); return; }
    if (startswith_ci(line, "ENC")) {
        const char *arg = line + 3;
        while (*arg == ' ' || *arg == '\t') arg++;
        if (startswith_ci(arg, "RESET")) {
            encoder_cmd_reset();
            printf("[ENC] counters zeroed\n");
            return;
        }
        char sum[80];
        encoder_cmd_status_summary(sum, sizeof(sum));
        printf("[ENC] %s\n", sum);
        encoder_cmd_dump();
        return;
    }
    if (startswith_ci(line, "LVGL_SCREENSHOT")) {
        extern bool lvgl_ui_display_is_up(void);
        if (!lvgl_ui_display_is_up()) {
            printf("[SHOT] LVGL is off -- enable with LVGL_FORCE ON + reboot\n");
            return;
        }
        char path[128] = {0};
        esp_err_t r = lvgl_ui_screenshot_write(path, sizeof(path));
        if (r == ESP_OK) {
            printf("[SHOT] wrote %s\n", path);
        } else {
            printf("[SHOT] capture failed: %s\n", esp_err_to_name(r));
        }
        return;
    }
    if (startswith_ci(line, "RTC")) {
        // Aggregator verb per Module_Blueprint. Existing GET_TIME / SET_TIME /
        // RTC_DUMP verbs still work (matched above); this is the module-scoped
        // shortcut for status + dump.
        const char *arg = line + 3;
        while (*arg == ' ' || *arg == '\t') arg++;
        if (*arg == 0) {
            char sum[80];
            pcf85063_cmd_status_summary(sum, sizeof(sum));
            printf("[RTC] %s\n", sum);
            pcf85063_cmd_dump();
            return;
        }
        printf("[RTC] usage: RTC (no arg = summary + dump). "
               "Use GET_TIME / SET_TIME / RTC_DUMP for direct verbs.\n");
        return;
    }
    if (startswith_ci(line, "FS_LS")) {
        const char *arg = line + 5;
        while (*arg == ' ' || *arg == '\t') arg++;
        rtc_cli_fs_ls(*arg ? arg : NULL);
        return;
    }
    if (startswith_ci(line, "FS_CAT")) {
        const char *arg = line + 6;
        while (*arg == ' ' || *arg == '\t') arg++;
        rtc_cli_fs_cat(arg);
        return;
    }
    if (startswith_ci(line, "DISP")) {
        extern bool      boot_display_is_present(void);
        extern bool      boot_display_is_asleep(void);
        extern esp_err_t boot_display_sleep(void);
        extern esp_err_t boot_display_wake(void);
        const char *arg = line + 4;
        while (*arg == ' ' || *arg == '\t') arg++;
        if (!boot_display_is_present()) {
            printf("[DISP] panel absent -- nothing to sleep/wake\n");
            return;
        }
        if (*arg == 0) {
            printf("[DISP] panel = %s\n", boot_display_is_asleep() ? "asleep" : "awake");
            return;
        }
        if (startswith_ci(arg, "OFF")) {
            ESP_LOGI("FC_CLI", "[DISP] reason=cli DISP OFF");
            esp_err_t e = boot_display_sleep();
            printf("[DISP] sleep %s\n", (e == ESP_OK) ? "OK" : esp_err_to_name(e));
            return;
        }
        if (startswith_ci(arg, "ON")) {
            ESP_LOGI("FC_CLI", "[DISP] reason=cli DISP ON");
            esp_err_t e = boot_display_wake();
            // Restart the 15s idle timer so the auto-sleep loop doesn't
            // immediately re-sleep right after an operator wake.
            s_last_activity_ms = millis_u32();
            printf("[DISP] wake %s\n", (e == ESP_OK) ? "OK" : esp_err_to_name(e));
            return;
        }
        printf("[DISP] usage: DISP [ON|OFF] (no arg = state)\n");
        return;
    }
    if (startswith_ci(line, "FUEL")) {
        uint16_t version = 0, vcell_mv = 0, soc_pct100 = 0;
        esp_err_t rv = ESP_FAIL, rc = ESP_FAIL, rs = ESP_FAIL;
        if (xSemaphoreTake(g_i2c2_mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
            rv = max17048_read16(I2C_NUM_1, MAX17048_REG_VERSION, &version);
            rc = max17048_read_vcell_mv(I2C_NUM_1, &vcell_mv);
            rs = max17048_read_soc_pct100(I2C_NUM_1, &soc_pct100);
            xSemaphoreGive(g_i2c2_mutex);
        } else {
            printf("[FUEL] g_i2c2_mutex timeout -- try again\n");
            return;
        }
        if (rv != ESP_OK) {
            printf("[FUEL] MAX17048 not responding (%s) -- no cell attached?\n",
                   esp_err_to_name(rv));
            return;
        }
        printf("[FUEL] VER=0x%04X  VCELL=%u mV  FUEL=%u.%02u %%  (reads=%s/%s)\n",
               version, vcell_mv, soc_pct100 / 100U, soc_pct100 % 100U,
               (rc == ESP_OK) ? "OK" : "ERR",
               (rs == ESP_OK) ? "OK" : "ERR");
        return;
    }
    if (startswith_ci(line, "RGB")) {
        const char *arg = line + 3;
        while (*arg == ' ' || *arg == '\t') arg++;
        if (*arg == 0) {
            // Bare `RGB` = status + dump (Module_Blueprint convention).
            char sum[80];
            ws2812_cmd_status_summary(sum, sizeof(sum));
            printf("[RGB] %s\n", sum);
            ws2812_cmd_dump();
            return;
        }
        if (startswith_ci(arg, "AUTO") || startswith_ci(arg, "RESET")) {
            ws2812_cmd_auto();
            printf("[RGB] manual override cleared -- rgb_policy resumed\n");
            return;
        }
        int r=-1, g=-1, b=-1;
        if (sscanf(arg, "%d %d %d", &r, &g, &b) == 3 &&
            r >= 0 && r <= 255 && g >= 0 && g <= 255 && b >= 0 && b <= 255) {
            ws2812_cmd_set_rgb((uint8_t)r, (uint8_t)g, (uint8_t)b);
            printf("[RGB] set to (%d, %d, %d) -- policy paused; RGB AUTO to release\n",
                   r, g, b);
        } else {
            printf("[RGB] usage: RGB <r> <g> <b>   or   RGB AUTO   (no arg = dump)\n");
        }
        return;
    }
    if (startswith_ci(line, "SHIPMODE")) {
        printf("[SHIP] triggering ship mode -- BATFET drops in ~10 s. "
               "Unplug USB (if attached) to complete shutdown.\n");
        fflush(stdout);
        watcher_ship_mode();
        printf("[SHIP] BATFET dropped but device still powered by USB. "
               "Unplug USB to finish shutdown.\n");
        return;
    }
    if (startswith_ci(line, "FLASH")) {
        /* Direct flashlight toggle. Bypasses the FCM_FLASHLIGHT mode-cycle
         * so the LED works while the encoder is offline on the iv8.0 unit.
         * project_encoder_dead_on_iv80.
         */
        const char *arg = line + 5;
        while (*arg == ' ' || *arg == '\t') arg++;
        if (*arg == 0) {
            printf("[FLASH] brightness = %u%%\n",
                   (unsigned)flashlight_get_brightness());
        } else if (startswith_ci(arg, "ON")) {
            esp_err_t r = flashlight_set_brightness(100);
            printf("[FLASH] ON (100%%) -- %s\n", esp_err_to_name(r));
        } else if (startswith_ci(arg, "OFF")) {
            esp_err_t r = flashlight_set_brightness(0);
            printf("[FLASH] OFF -- %s\n", esp_err_to_name(r));
        } else {
            int pct = atoi(arg);
            if (pct < 0 || pct > 100) {
                printf("[FLASH] usage: FLASH [ON|OFF|<0..100>]\n");
            } else {
                esp_err_t r = flashlight_set_brightness((uint8_t)pct);
                printf("[FLASH] brightness = %d%% -- %s\n",
                       pct, esp_err_to_name(r));
            }
        }
        return;
    }
    if (startswith_ci(line, "AUTOSHDN")) {
        const char *arg = line + 8;
        while (*arg == ' ' || *arg == '\t') arg++;
        if (*arg == 0) {
            uint16_t m = nvs_cfg_sys_get_auto_shdn_min();
            if (m == 0) {
                printf("[SHDN] auto_shdn = OFF (perma-on)\n");
            } else {
                printf("[SHDN] auto_shdn = %u min\n", (unsigned)m);
            }
        } else if (startswith_ci(arg, "OFF")) {
            esp_err_t r = nvs_cfg_sys_set_auto_shdn_min(0);
            printf("[SHDN] auto_shdn = OFF (%s). Reboot to apply.\n",
                   esp_err_to_name(r));
        } else {
            int m = atoi(arg);
            if (m < 0 || m > 1440) {
                printf("[SHDN] usage: AUTOSHDN <0..1440> | OFF  (0/OFF = perma-on)\n");
            } else {
                esp_err_t r = nvs_cfg_sys_set_auto_shdn_min((uint16_t)m);
                if (m == 0) {
                    printf("[SHDN] auto_shdn = OFF (%s). Reboot to apply.\n",
                           esp_err_to_name(r));
                } else {
                    printf("[SHDN] auto_shdn = %d min (%s). Reboot to apply.\n",
                           m, esp_err_to_name(r));
                }
            }
        }
        return;
    }
    if (startswith_ci(line, "BLACKBOX_CADENCE")) {
        const char *arg = line + 16;
        while (*arg == ' ' || *arg == '\t') arg++;
        if (*arg == 0) {
            printf("[BB] cadence = %u s\n", (unsigned)nvs_cfg_sys_get_bb_cadence_s());
        } else {
            int s = atoi(arg);
            if (s < 1 || s > 3600) {
                printf("[BB] usage: BLACKBOX_CADENCE <1..3600>\n");
            } else {
                esp_err_t r = nvs_cfg_sys_set_bb_cadence_s((uint16_t)s);
                printf("[BB] cadence = %d s (%s). Takes effect on next sample tick.\n",
                       s, esp_err_to_name(r));
            }
        }
        return;
    }
    if (startswith_ci(line, "BLACKBOX")) {
        const char *arg = line + 8;
        while (*arg == ' ' || *arg == '\t') arg++;
        if (startswith_ci(arg, "ON")) {
            esp_err_t r = nvs_cfg_sys_set_blackbox(true);
            printf("[BB] blackbox=1 (%s). Reboot to start task.\n", esp_err_to_name(r));
        } else if (startswith_ci(arg, "OFF")) {
            esp_err_t r = nvs_cfg_sys_set_blackbox(false);
            printf("[BB] blackbox=0 (%s). Reboot to stop task.\n", esp_err_to_name(r));
        } else if (*arg == 0) {
            printf("[BB] blackbox = %d  cadence=%u s\n",
                   nvs_cfg_sys_get_blackbox() ? 1 : 0,
                   (unsigned)nvs_cfg_sys_get_bb_cadence_s());
        } else {
            printf("[BB] usage: BLACKBOX [ON|OFF]  (no arg = show state)\n");
        }
        return;
    }
    if (startswith_ci(line, "REC_AUDIO")) {
        const char *arg = line + 9;
        while (*arg == ' ' || *arg == '\t') arg++;
        if (startswith_ci(arg, "ON")) {
            esp_err_t r = nvs_cfg_sys_set_rec_audio(true);
            printf("[REC] rec_audio=1 (%s). Next recording will play 5 s mic annotation.\n",
                   esp_err_to_name(r));
        } else if (startswith_ci(arg, "OFF")) {
            esp_err_t r = nvs_cfg_sys_set_rec_audio(false);
            printf("[REC] rec_audio=0 (%s). Next recording will skip mic annotation.\n",
                   esp_err_to_name(r));
        } else if (*arg == 0) {
            printf("[REC] rec_audio = %d\n", nvs_cfg_sys_get_rec_audio() ? 1 : 0);
        } else {
            printf("[REC] usage: REC_AUDIO [ON|OFF]  (no arg = show state)\n");
        }
        return;
    }
    if (startswith_ci(line, "BQ")) {
        const char *arg = line + 2;
        while (*arg == ' ' || *arg == '\t') arg++;

        if (*arg == 0) {
            char sum[96];
            bq_cmd_status_summary(sum, sizeof(sum));
            printf("[BQ] %s\n", sum);
            bq_cmd_dump();
            return;
        }
        if (startswith_ci(arg, "SHIPMODE")) {
            printf("[BQ] SHIPMODE alias -- firing watcher_ship_mode (buzz + BATFET drop)\n");
            fflush(stdout);
            watcher_ship_mode();
            return;
        }
        if (startswith_ci(arg, "EN")) {
            const char *val = arg + 2;
            while (*val == ' ' || *val == '\t') val++;
            if (*val == 0) {
                printf("[BQ] charger enable = %d\n", bq_cmd_enable_get() ? 1 : 0);
            } else if (startswith_ci(val, "ON")) {
                esp_err_t r = bq_cmd_enable_set(true);
                printf("[BQ] enable=1 (%s)\n", esp_err_to_name(r));
            } else if (startswith_ci(val, "OFF")) {
                esp_err_t r = bq_cmd_enable_set(false);
                printf("[BQ] enable=0 (%s)\n", esp_err_to_name(r));
            } else {
                printf("[BQ] usage: BQ EN [ON|OFF]\n");
            }
            return;
        }
        if (startswith_ci(arg, "BOOST")) {
            const char *val = arg + 5;
            while (*val == ' ' || *val == '\t') val++;
            if (*val == 0) {
                printf("[BQ] PMID boost = %d\n", bq_cmd_boost_get() ? 1 : 0);
            } else if (startswith_ci(val, "ON")) {
                esp_err_t r = bq_cmd_boost_set(true);
                printf("[BQ] boost=1 (%s)\n", esp_err_to_name(r));
            } else if (startswith_ci(val, "OFF")) {
                esp_err_t r = bq_cmd_boost_set(false);
                printf("[BQ] boost=0 (%s)\n", esp_err_to_name(r));
            } else {
                printf("[BQ] usage: BQ BOOST [ON|OFF]\n");
            }
            return;
        }
        printf("[BQ] usage: BQ [EN|BOOST [ON|OFF] | SHIPMODE]  (no arg = dump)\n");
        return;
    }
    if (startswith_ci(line, "HAPTIC")) {
        const char *arg = line + 6;
        while (*arg == ' ' || *arg == '\t') arg++;

        if (*arg == 0) {
            char sum[96];
            drv2605_cmd_status_summary(sum, sizeof(sum));
            printf("[HAPTIC] %s\n", sum);
            drv2605_cmd_dump();
            return;
        }
        if (startswith_ci(arg, "PLAY")) {
            const char *val = arg + 4;
            while (*val == ' ' || *val == '\t') val++;
            if (*val == 0) {
                printf("[HAPTIC] usage: HAPTIC PLAY <1..123>\n");
            } else {
                int n = atoi(val);
                esp_err_t r = drv2605_cmd_play_effect((uint8_t)n);
                printf("[HAPTIC] play(%d) -> %s\n", n, esp_err_to_name(r));
            }
            return;
        }
        if (startswith_ci(arg, "CAL")) {
            drv2605_cmd_calibrate();
            printf("[HAPTIC] calibration requested (auto-cal is diagnostic; expect DIAG fail on Taptic)\n");
            return;
        }
        if (startswith_ci(arg, "SWEEP")) {
            const char *val = arg + 5;
            while (*val == ' ' || *val == '\t') val++;
            if (startswith_ci(val, "START")) {
                drv2605_cmd_sweep_start();
                printf("[HAPTIC] sweep START\n");
            } else if (startswith_ci(val, "STOP")) {
                drv2605_cmd_sweep_stop();
                printf("[HAPTIC] sweep STOP (latched current step)\n");
            } else {
                printf("[HAPTIC] usage: HAPTIC SWEEP START|STOP\n");
            }
            return;
        }
        if (startswith_ci(arg, "EN")) {
            const char *val = arg + 2;
            while (*val == ' ' || *val == '\t') val++;
            if (*val == 0) {
                printf("[HAPTIC] enable = %d\n", drv2605_cmd_enable_get() ? 1 : 0);
            } else if (startswith_ci(val, "ON")) {
                esp_err_t r = drv2605_cmd_enable_set(true);
                printf("[HAPTIC] enable=1 (%s)\n", esp_err_to_name(r));
            } else if (startswith_ci(val, "OFF")) {
                esp_err_t r = drv2605_cmd_enable_set(false);
                printf("[HAPTIC] enable=0 (%s)\n", esp_err_to_name(r));
            } else {
                printf("[HAPTIC] usage: HAPTIC EN [ON|OFF]\n");
            }
            return;
        }
        if (startswith_ci(arg, "UI")) {
            const char *val = arg + 2;
            while (*val == ' ' || *val == '\t') val++;
            if (*val == 0) {
                printf("[HAPTIC] ui_effect = %u\n", (unsigned)drv2605_cmd_ui_effect_get());
            } else {
                int n = atoi(val);
                if (n < 1 || n > 123) {
                    printf("[HAPTIC] usage: HAPTIC UI <1..123>\n");
                } else {
                    drv2605_cmd_ui_effect_set((uint8_t)n);
                    printf("[HAPTIC] ui_effect=%d (previewed + saved)\n", n);
                }
            }
            return;
        }
        printf("[HAPTIC] usage: HAPTIC [EN|PLAY <n>|CAL|SWEEP START|STOP|UI [<n>]]  (no arg = dump)\n");
        return;
    }
    if (startswith_ci(line, "LIGHT")) {
        const char *arg = line + 5;
        while (*arg == ' ' || *arg == '\t') arg++;

        if (*arg == 0) {
            char sum[96];
            veml6030_cmd_status_summary(sum, sizeof(sum));
            printf("[LIGHT] %s\n", sum);
            veml6030_cmd_dump();
            return;
        }
        // Match longer prefixes before shorter ones (BLUE before BR, AUTO
        // before nothing, etc.). "EN" is only 2 chars and could ambiguate
        // with a hypothetical "EFFECT" someday -- guard with a trailing
        // space/EOL check.
        if (startswith_ci(arg, "AUTO")) {
            const char *val = arg + 4;
            while (*val == ' ' || *val == '\t') val++;
            if (*val == 0) {
                printf("[LIGHT] auto_brightness = %d\n",
                       veml6030_cmd_auto_brightness_get() ? 1 : 0);
            } else if (startswith_ci(val, "ON")) {
                veml6030_cmd_auto_brightness_set(true);
                printf("[LIGHT] auto=1 (saved async)\n");
            } else if (startswith_ci(val, "OFF")) {
                veml6030_cmd_auto_brightness_set(false);
                printf("[LIGHT] auto=0 (saved async)\n");
            } else {
                printf("[LIGHT] usage: LIGHT AUTO [ON|OFF]\n");
            }
            return;
        }
        if (startswith_ci(arg, "BLUE")) {
            const char *val = arg + 4;
            while (*val == ' ' || *val == '\t') val++;
            if (*val == 0) {
                printf("[LIGHT] blue_light_filter = %d\n",
                       veml6030_cmd_blue_light_get() ? 1 : 0);
            } else if (startswith_ci(val, "ON")) {
                veml6030_cmd_blue_light_set(true);
                printf("[LIGHT] blue=1 (overlay flips on next tile update)\n");
            } else if (startswith_ci(val, "OFF")) {
                veml6030_cmd_blue_light_set(false);
                printf("[LIGHT] blue=0\n");
            } else {
                printf("[LIGHT] usage: LIGHT BLUE [ON|OFF]\n");
            }
            return;
        }
        if (startswith_ci(arg, "BR")) {
            const char *val = arg + 2;
            while (*val == ' ' || *val == '\t') val++;
            if (*val == 0) {
                printf("[LIGHT] brightness = %u%%\n",
                       (unsigned)veml6030_cmd_brightness_get());
            } else {
                int n = atoi(val);
                if (n < 1 || n > 100) {
                    printf("[LIGHT] usage: LIGHT BR <1..100>\n");
                } else {
                    esp_err_t r = veml6030_cmd_brightness_set((uint8_t)n);
                    printf("[LIGHT] brightness=%d%% (%s%s)\n", n,
                           esp_err_to_name(r),
                           veml6030_cmd_auto_brightness_get()
                             ? ", auto still on (saved; visible on AUTO OFF)" : "");
                }
            }
            return;
        }
        if (startswith_ci(arg, "EN")) {
            const char *val = arg + 2;
            while (*val == ' ' || *val == '\t') val++;
            if (*val == 0) {
                printf("[LIGHT] sensor enable = %d\n",
                       veml6030_cmd_enable_get() ? 1 : 0);
            } else if (startswith_ci(val, "ON")) {
                veml6030_cmd_enable_set(true);
                printf("[LIGHT] enable=1\n");
            } else if (startswith_ci(val, "OFF")) {
                veml6030_cmd_enable_set(false);
                printf("[LIGHT] enable=0\n");
            } else {
                printf("[LIGHT] usage: LIGHT EN [ON|OFF]\n");
            }
            return;
        }
        printf("[LIGHT] usage: LIGHT [EN|AUTO|BLUE [ON|OFF] | BR [<0..100>]]  (no arg = dump)\n");
        return;
    }
    if (startswith_ci(line, "BATT_TEST")) {
        const char *arg = line + 9;
        while (*arg == ' ' || *arg == '\t') arg++;
        if (startswith_ci(arg, "ON")) {
            esp_err_t r = nvs_cfg_sys_set_batt_test(true);
            printf("[BATT] batt_test=1 (%s). Reboot to enter mode.\n",
                   esp_err_to_name(r));
        } else if (startswith_ci(arg, "OFF")) {
            esp_err_t r = nvs_cfg_sys_set_batt_test(false);
            printf("[BATT] batt_test=0 (%s). Reboot to return to normal FCM.\n",
                   esp_err_to_name(r));
        } else if (*arg == 0) {
            printf("[BATT] batt_test = %d\n",
                   nvs_cfg_sys_get_batt_test() ? 1 : 0);
        } else {
            printf("[BATT] usage: BATT_TEST [ON|OFF]  (no arg = show state)\n");
        }
        return;
    }
    if (startswith_ci(line, "GPS_VIEW")) {
        // GPS_VIEW is a per-tile mode setter; typing here dispatches to the
        // same gps_tile_cmd_view_set() that the in-tile PHOTO button calls.
        // Both outlets share one code path (Module_Blueprint.md §6).
        extern bool lvgl_ui_display_is_up(void);
        extern bool lvgl_port_lock(uint32_t);
        extern void lvgl_port_unlock(void);
        // Enum values must match gps_tile.h GPS_TILE_VIEW_* (0=normal, 1=photo).
        extern void gps_tile_cmd_view_set(int v);
        extern void gps_tile_cmd_view_toggle(void);
        extern int  gps_tile_cmd_view_get(void);

        if (!lvgl_ui_display_is_up()) {
            printf("[GPS] LVGL is off -- enable with LVGL_FORCE ON + reboot\n");
            return;
        }
        const char *arg = line + 8;
        while (*arg == ' ' || *arg == '\t') arg++;

        int action = -1;              // 0=set normal, 1=set photo, 2=toggle, 3=get
        if      (*arg == 0)                          action = 3;
        else if (startswith_ci(arg, "NORMAL"))       action = 0;
        else if (startswith_ci(arg, "PHOTO"))        action = 1;
        else if (startswith_ci(arg, "TOGGLE"))       action = 2;
        else if (startswith_ci(arg, "STATUS") ||
                 startswith_ci(arg, "GET"))          action = 3;

        if (action < 0) {
            printf("[GPS] usage: GPS_VIEW [normal|photo|toggle]  (no arg = show state)\n");
            return;
        }

        if (action != 3) {
            if (!lvgl_port_lock(pdMS_TO_TICKS(100))) {
                printf("[GPS] lvgl_port_lock timeout -- try again\n");
                return;
            }
            if      (action == 0) gps_tile_cmd_view_set(0);
            else if (action == 1) gps_tile_cmd_view_set(1);
            else                  gps_tile_cmd_view_toggle();
            lvgl_port_unlock();
        }

        const int v = gps_tile_cmd_view_get();
        printf("[GPS] view = %s\n", (v == 1) ? "PHOTO" : "NORMAL");
        return;
    }
    if (startswith_ci(line, "GPS_DYNMODEL")) {
        // Stage 31.4b: set MAX-M10S dynamic platform model at runtime.
        // max_m10s.h is reachable via data_broker.h include chain -- use
        // the real types directly instead of re-externing (was causing
        // conflicting-type build errors).
        const char *arg = line + 12;
        while (*arg == ' ' || *arg == '\t') arg++;

        int mode = -1;
        if      (*arg == 0)                          { /* show current */ }
        else if (startswith_ci(arg, "PORT"))         mode = UBX_DYNMODEL_PORTABLE;
        else if (startswith_ci(arg, "STAT"))         mode = UBX_DYNMODEL_STATIONARY;
        else if (startswith_ci(arg, "PED"))          mode = UBX_DYNMODEL_PEDESTRIAN;
        else if (startswith_ci(arg, "AUTO"))         mode = UBX_DYNMODEL_AUTOMOTIVE;
        else if (startswith_ci(arg, "SEA"))          mode = UBX_DYNMODEL_SEA;
        else if (startswith_ci(arg, "AIR1"))         mode = UBX_DYNMODEL_AIR1G;
        else if (startswith_ci(arg, "AIR2"))         mode = UBX_DYNMODEL_AIR2G;
        else if (startswith_ci(arg, "AIR4"))         mode = UBX_DYNMODEL_AIR4G;
        else if (startswith_ci(arg, "WRIST"))        mode = UBX_DYNMODEL_WRIST;
        else if (startswith_ci(arg, "BIKE"))         mode = UBX_DYNMODEL_BIKE;
        else {
            printf("[GPS] usage: GPS_DYNMODEL "
                   "[PORT|STAT|PED|AUTO|SEA|AIR1|AIR2|AIR4|WRIST|BIKE]"
                   "  (no arg = show current)\n");
            return;
        }

        if (mode >= 0) {
            esp_err_t r = max_m10s_set_dynmodel((max_m10s_dynmodel_t)mode);
            printf("[GPS] DYNMODEL <- %s (%d) : %s\n",
                   max_m10s_dynmodel_name((max_m10s_dynmodel_t)mode),
                   mode, esp_err_to_name(r));
            printf("      watch DEBUG log for UBX-ACK-NAK if not supported.\n");
        }
        max_m10s_dynmodel_t cur = max_m10s_get_dynmodel();
        printf("[GPS] DYNMODEL = %s (%d)\n",
               max_m10s_dynmodel_name(cur), (int)cur);
        return;
    }
    if (startswith_ci(line, "GPS_MONRF")) {
        // Stage 31.4b: dump UBX-MON-RF snapshot. Optional "ON" arg re-issues
        // the enable frame so we can retry after the chip has fully booted
        // (useful if the init-time enable was lost for whatever reason).
        const char *arg = line + 9;   // strlen("GPS_MONRF")
        while (*arg == ' ' || *arg == '\t') arg++;
        if (*arg && startswith_ci(arg, "ON")) {
            esp_err_t r = max_m10s_enable_monrf(1);
            printf("[GPS] MON-RF enable re-issued: %s\n", esp_err_to_name(r));
            return;
        }
        max_m10s_monrf_t rf = {0};
        max_m10s_get_monrf(&rf);
        if (!rf.valid) {
            printf("[GPS] MON-RF: no snapshot yet -- either MON-RF not enabled\n");
            printf("             or chip is silent (check antenna/wiring first).\n");
            printf("             Try: GPS_PING (proves TX), then GPS_MONRF ON.\n");
            return;
        }
        uint32_t age_ms = (uint32_t)(esp_timer_get_time() / 1000ULL)
                        - rf.last_update_ms;
        const char *pwr_str = (rf.ant_power == 0) ? "OFF" :
                              (rf.ant_power == 1) ? "ON"  :
                              (rf.ant_power == 2) ? "DONTKNOW" : "?";
        int agc_pct = (int)(((uint32_t)rf.agc_cnt * 100) / 8191U);
        printf("[GPS] MON-RF (age=%lu ms):\n", (unsigned long)age_ms);
        printf("      antStatus  = %s (%u)\n",
               max_m10s_ant_status_name(rf.ant_status), rf.ant_status);
        printf("      antPower   = %s (%u)\n", pwr_str, rf.ant_power);
        printf("      noise/ms   = %u\n", rf.noise_per_ms);
        printf("      AGC count  = %u (~%d%%)\n", rf.agc_cnt, agc_pct);
        return;
    }
    if (startswith_ci(line, "GPS_PING")) {
        max_m10s_ubx_counters_t before = {0};
        max_m10s_get_ubx_counters(&before);
        esp_err_t r = max_m10s_ping();
        if (r != ESP_OK) {
            printf("[GPS] PING send failed: %s\n", esp_err_to_name(r));
            return;
        }
        // Chip typically responds in ~5-10 ms; poll for up to 300 ms.
        for (int i = 0; i < 30; i++) {
            vTaskDelay(pdMS_TO_TICKS(10));
            max_m10s_ubx_counters_t now = {0};
            max_m10s_get_ubx_counters(&now);
            if (now.mon_ver > before.mon_ver) {
                printf("[GPS] PING -> MON-VER response received (TX path OK)\n");
                return;
            }
        }
        printf("[GPS] PING: NO response in 300 ms.\n");
        printf("      TX path to GPS is broken, OR chip is not powered / not booted.\n");
        printf("      NMEA still flowing? Then RX works, only TX side is dead.\n");
        return;
    }
    if (startswith_ci(line, "GPS_UBX_STATS")) {
        max_m10s_ubx_counters_t c = {0};
        max_m10s_get_ubx_counters(&c);
        printf("[GPS] UBX counters:  total=%lu  mon_ver=%lu  mon_rf=%lu  "
               "nav_timeutc=%lu  ack_ack=%lu  ack_nak=%lu\n",
               (unsigned long)c.total, (unsigned long)c.mon_ver,
               (unsigned long)c.mon_rf, (unsigned long)c.nav_timeutc,
               (unsigned long)c.ack_ack, (unsigned long)c.ack_nak);
        if (c.total == 0) {
            printf("      All zero: either TX broken (chip never got a request/enable) "
                   "or every UBX frame is being dropped upstream.\n");
        }
        return;
    }
    if (startswith_ci(line, "GPS_SNR")) {
        max_m10s_snr_summary_t s = {0};
        max_m10s_get_snr_summary(&s);
        printf("[GPS] SNR (from NMEA GSV, last %u ms window):\n", 2500);
        printf("      sats_with_snr = %u\n", s.sats_with_snr);
        printf("      max CN0       = %u dBHz\n", s.max_cn0);
        printf("      top-4 avg     = %u dBHz\n", s.top4_avg_cn0);
        return;
    }
    if (startswith_ci(line, "TOUCH")) {
        extern bool boot_display_touch_is_present(void);
        extern bool lvgl_ui_display_touch_indev_ready(void);
        extern void lvgl_ui_display_touch_snapshot(uint16_t *x, uint16_t *y,
                                                   uint32_t *ev, bool *pressed);
        if (!boot_display_touch_is_present()) {
            printf("[TOUCH] CST9217 absent -- no touch present\n");
            return;
        }
        uint16_t x = 0, y = 0;
        uint32_t ev = 0;
        bool pressed = false;
        lvgl_ui_display_touch_snapshot(&x, &y, &ev, &pressed);
        printf("[TOUCH] indev=%s  last=(%u,%u)  events=%lu  now=%s\n",
               lvgl_ui_display_touch_indev_ready() ? "bound" : "off",
               (unsigned)x, (unsigned)y,
               (unsigned long)ev,
               pressed ? "PRESSED" : "released");
        return;
    }
    if (startswith_ci(line, "TILE")) {
        // Tile column ordering mirrors components/lvgl_ui/tile_registry.c.
        // Names are hard-coded here rather than added to tile_desc_t so no
        // widget file needs touching -- an eventual `name` descriptor field
        // would obsolete this table (revisit next time tile_desc_t is touched
        // per feedback_group_work_by_venue.md).
        static const char *TILE_NAMES[] = {
            "health",   // col 0
            "haptic",   // col 1
            "light",    // col 2
            "system",   // col 3
            "gps",      // col 4
            "rtc",      // col 5
            "env",      // col 6
            "compass",  // col 7
            "imu",      // col 8
            "ecg",      // col 9
        };
        const int TILE_NAME_COUNT = (int)(sizeof(TILE_NAMES) / sizeof(TILE_NAMES[0]));

        extern int       lvgl_ui_display_tile_count(void);
        extern esp_err_t lvgl_ui_display_jump_tile(int col);
        extern bool      lvgl_ui_display_is_up(void);

        if (!lvgl_ui_display_is_up()) {
            printf("[TILE] LVGL is off -- enable with LVGL_FORCE ON + reboot, or plug a real panel\n");
            return;
        }

        const char *arg = line + 4;
        while (*arg == ' ' || *arg == '\t') arg++;

        if (*arg == 0 || startswith_ci(arg, "LIST")) {
            const int n = lvgl_ui_display_tile_count();
            printf("[TILE] %d tiles registered:\n", n);
            for (int i = 0; i < n; i++) {
                const char *nm = (i < TILE_NAME_COUNT) ? TILE_NAMES[i] : "(unnamed)";
                printf("  col %d  %s\n", i, nm);
            }
            return;
        }

        int col = -1;
        // Numeric first.
        char *endp = NULL;
        long lv = strtol(arg, &endp, 10);
        if (endp != arg && *endp <= ' ') {
            col = (int)lv;
        } else {
            // Name match.
            for (int i = 0; i < TILE_NAME_COUNT; i++) {
                if (startswith_ci(arg, TILE_NAMES[i])) { col = i; break; }
            }
        }
        if (col < 0) {
            printf("[TILE] usage: TILE list | <n> | <name>  (e.g. TILE 4  or  TILE gps)\n");
            return;
        }
        esp_err_t r = lvgl_ui_display_jump_tile(col);
        if (r == ESP_OK) {
            const char *nm = (col < TILE_NAME_COUNT) ? TILE_NAMES[col] : "(unnamed)";
            printf("[TILE] jumped to col %d (%s)\n", col, nm);
        } else {
            printf("[TILE] jump failed: %s\n", esp_err_to_name(r));
        }
        return;
    }
    if (startswith_ci(line, "LVGL_FORCE")) {
        const char *arg = line + 10;
        while (*arg == ' ' || *arg == '\t') arg++;
        if (startswith_ci(arg, "ON")) {
            esp_err_t r = nvs_cfg_sys_set_lvgl_force_on(true);
            printf("[LVGL] lvgl_force=1 (%s). Reboot to bring the LVGL side up "
                   "without a panel; flush callback becomes a no-op.\n",
                   esp_err_to_name(r));
        } else if (startswith_ci(arg, "OFF")) {
            esp_err_t r = nvs_cfg_sys_set_lvgl_force_on(false);
            printf("[LVGL] lvgl_force=0 (%s). Reboot to return to probe-gated "
                   "behaviour (default).\n",
                   esp_err_to_name(r));
        } else if (*arg == 0) {
            printf("[LVGL] lvgl_force = %d\n",
                   nvs_cfg_sys_get_lvgl_force_on() ? 1 : 0);
        } else {
            printf("[LVGL] usage: LVGL_FORCE [ON|OFF]  (no arg = show state)\n");
        }
        return;
    }
    if (startswith_ci(line, "GESTURE")) {
        const char *NAMES[] = {"NONE", "WRIST_RAISE", "WRIST_DOWN", "SHAKE"};
        uint8_t g = g_imu_gesture;
        const char *n = (g < 4) ? NAMES[g] : "unknown";
        uint32_t last_ms  = lsm6dsv16x_gesture_last_change_ms();
        uint32_t now_ms   = millis_u32();
        uint32_t age_ms   = last_ms ? (now_ms - last_ms) : 0;
        printf("[GESTURE] current=%s (%u)  az_lpf=%.3f g  changes=%lu  "
               "last_change_age=%lu ms\n",
               n, (unsigned)g,
               (double)lsm6dsv16x_gesture_az_lpf_g(),
               (unsigned long)lsm6dsv16x_gesture_change_count(),
               (unsigned long)age_ms);
        return;
    }
    if (startswith_ci(line, "LOGLEVEL")) {
        const char *arg = line + 8;
        while (*arg == ' ' || *arg == '\t') arg++;

        // Encoding matches ESP_LOG_* / nvs_cfg_sys_*_log_level().
        static const char *NAMES[] = {"NONE", "ERROR", "WARN", "INFO",
                                       "DEBUG", "VERBOSE"};

        if (*arg == 0) {
            uint8_t stored = nvs_cfg_sys_get_log_level();
            const char *n = (stored <= 5) ? NAMES[stored]
                            : (stored == 0xFF ? "AUTO" : "unknown");
            printf("[LOG] stored=%s effective ESP_LOG level = %d\n",
                   n, (int)esp_log_get_default_level());
            return;
        }

        int lv = -1;
        if      (startswith_ci(arg, "OFF")     || startswith_ci(arg, "NONE"))    lv = ESP_LOG_NONE;
        else if (startswith_ci(arg, "ERROR")   || startswith_ci(arg, "E"))       lv = ESP_LOG_ERROR;
        else if (startswith_ci(arg, "WARN")    || startswith_ci(arg, "W"))       lv = ESP_LOG_WARN;
        else if (startswith_ci(arg, "INFO")    || startswith_ci(arg, "I"))       lv = ESP_LOG_INFO;
        else if (startswith_ci(arg, "DEBUG")   || startswith_ci(arg, "D"))       lv = ESP_LOG_DEBUG;
        else if (startswith_ci(arg, "VERBOSE") || startswith_ci(arg, "V"))       lv = ESP_LOG_VERBOSE;
        else if (startswith_ci(arg, "AUTO")) {
            // Reset persisted value to the sentinel; next boot picks the
            // USB-vs-battery default. Current session goes to INFO for
            // parity with the fresh-boot default.
            (void)nvs_cfg_sys_set_log_level(0xFF);
            esp_log_level_set("*", ESP_LOG_INFO);
            printf("[LOG] AUTO -- next boot decides; this session at INFO\n");
            return;
        }

        if (lv < 0) {
            printf("[LOG] usage: LOGLEVEL [OFF|ERROR|WARN|INFO|DEBUG|VERBOSE|AUTO]\n");
            return;
        }

        esp_log_level_set("*", (esp_log_level_t)lv);
        esp_err_t r = nvs_cfg_sys_set_log_level((uint8_t)lv);
        printf("[LOG] level -> %s (%d)  persisted=%s\n",
               NAMES[lv], lv, esp_err_to_name(r));
        return;
    }
    if (!startswith_ci(line, "NVS_PRINT") && startswith_ci(line, "NVS")) {
        /* Match bare "NVS" -- exclude "NVS_PRINT" (handled below) via the
         * negated pre-check so this verb doesn't swallow its own prefix.
         */
        const char *arg = line + 3;
        while (*arg == ' ' || *arg == '\t') arg++;
        if (*arg == 0) {
            nvs_cfg_dump(I2C_NUM_0);
        } else {
            printf("[NVS] usage: NVS  (no args -- dumps every persisted record)\n");
        }
        return;
    }
    if (startswith_ci(line, "NVS_PRINT")) {
        const char *arg = line + 9;
        while (*arg == ' ' || *arg == '\t') arg++;
        if (startswith_ci(arg, "ON")) {
            esp_err_t r = nvs_cfg_sys_set_print_on_boot(true);
            printf("[NVS] print_on_boot=1 (%s)\n", esp_err_to_name(r));
        } else if (startswith_ci(arg, "OFF")) {
            esp_err_t r = nvs_cfg_sys_set_print_on_boot(false);
            printf("[NVS] print_on_boot=0 (%s)\n", esp_err_to_name(r));
        } else if (*arg == 0) {
            rtc_cli_dump_nvs();
            printf("[SYS] print_on_boot = %d\n",
                   nvs_cfg_sys_get_print_on_boot() ? 1 : 0);
        } else {
            printf("[NVS] usage: NVS_PRINT [ON|OFF]  (no arg = dump state)\n");
        }
        return;
    }
    // Unknown line -- ignore silently.
}

// ── Task entry point ────────────────────────────────────────────────────────
void task_rtc_cli_fn(void *arg) {
    (void)arg;

    usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    cfg.rx_buffer_size = 256;
    cfg.tx_buffer_size = 256;
    esp_err_t r = usb_serial_jtag_driver_install(&cfg);
    if (r != ESP_OK && r != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "usb_serial_jtag_driver_install failed: %s", esp_err_to_name(r));
        vTaskDelete(NULL);
        return;
    }

    vTaskDelay(pdMS_TO_TICKS(2000));
    fc_sync_init();
    printf("\n");
    printf("[RTC] Boot-time state:\n  ");
    rtc_cli_print_now();
    rtc_cli_print_help();

    static char  buf[80];
    static size_t len = 0;
    for (;;) {
        uint8_t c;
        int n = usb_serial_jtag_read_bytes(&c, 1, pdMS_TO_TICKS(100));
        if (n <= 0) continue;

        if (c == '\r' || c == '\n') {
            if (len > 0) {
                int64_t t_recv_us = esp_timer_get_time();
                buf[len] = 0;
                rtc_cli_handle_line(buf, t_recv_us);
                len = 0;
            }
            continue;
        }
        if (c == 0x08 || c == 0x7F) {
            if (len > 0) len--;
            continue;
        }
        if (len < sizeof(buf) - 1) buf[len++] = (char)c;
        else                       len = 0;
    }
}
