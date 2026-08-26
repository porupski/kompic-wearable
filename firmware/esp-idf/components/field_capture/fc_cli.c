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
#include "pcf85063.h"
#include "nvs_cfg.h"

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
static void rtc_cli_dump_nvs(void) {
    nvs_cfg_rtc_t r;
    if (nvs_cfg_rtc_load(&r) != ESP_OK || !r.valid) {
        printf("[NVS] cfg_rtc empty (SET_TIME has not been called since NVS erase)\n");
    } else {
        printf("[NVS] cfg_rtc.wall_ts       = %llu\n", (unsigned long long)r.wall_ts);
        printf("[NVS] cfg_rtc.wr_ms         = %llu\n", (unsigned long long)r.wr_ms);
        printf("[NVS] cfg_rtc.boot_seq      = %lu\n",  (unsigned long)r.boot_seq);
        printf("[NVS] cfg_rtc.last_set_time = \"%s\"\n", r.last_set_time);
    }
    uint8_t ram = 0;
    if (pcf85063_ram_byte_read(I2C_NUM_0, &ram) == ESP_OK) {
        printf("[PCF] RAM_byte (0x03)       = 0x%02X\n", ram);
    } else {
        printf("[PCF] RAM_byte read failed\n");
    }
    printf("[SYS] print_on_boot         = %d\n",
           nvs_cfg_sys_get_print_on_boot() ? 1 : 0);
    printf("[SYS] batt_test             = %d  (reboot to apply; VBUS-in keeps serial alive)\n",
           nvs_cfg_sys_get_batt_test() ? 1 : 0);
    printf("[SYS] blackbox              = %d  cadence=%u s\n",
           nvs_cfg_sys_get_blackbox() ? 1 : 0,
           (unsigned)nvs_cfg_sys_get_bb_cadence_s());
    printf("[SYS] rec_audio             = %d\n",
           nvs_cfg_sys_get_rec_audio() ? 1 : 0);
    char last_fw[NVS_CFG_FW_STR_MAX] = {0};
    (void)nvs_cfg_sys_get_last_fw(last_fw, sizeof(last_fw));
    printf("[SYS] last_fw               = \"%s\"  (current=%s)\n",
           last_fw[0] ? last_fw : "(none)", KOMPIC_FW_VERSION);
}

// ── HELP ────────────────────────────────────────────────────────────────────
static void rtc_cli_print_help(void) {
    printf("  Commands:\n");
    printf("    HELP                             this list\n");
    printf("    STATUS                           one-shot state dump (uptime, sensors, batt, heap)\n");
    printf("    GET_TIME [-v]                    read RTC now (-v also dumps NVS + RAM_byte)\n");
    printf("    SET_TIME YYYY-MM-DDTHH:MM:SS     write UTC + persist to NVS + PCF RAM_byte\n");
    printf("    RTC_DUMP                         hex dump of all 18 PCF85063A registers\n");
    printf("    NVS_PRINT [ON|OFF]               toggle boot-time NVS printout (no arg = dump)\n");
    printf("    BATT_TEST [ON|OFF]               enter battery-test mode on next boot (no arg = state)\n");
    printf("    BLACKBOX [ON|OFF]                background telemetry logger (reboot to start/stop)\n");
    printf("    BLACKBOX_CADENCE <s>             sample cadence in seconds (default 10, range 1..3600)\n");
    printf("    REC_AUDIO [ON|OFF]               5 s voice annotation before ENV/MOTION/SKIN (no arg = state)\n");
    printf("    WHOAMI                           I2C sensor identification + hw_alive status\n");
    printf("    TEMP_DUMP                        read every onboard temp source (waits for stable, non-zero)\n");
    printf("    PM_DUMP                          dump PM lock inventory now\n");
    printf("    RGB <r> <g> <b> | RGB AUTO       bench-poke WS2812 (0..255) / release override\n");
    printf("    FS_LS [/sd/path]                 list SD directory\n");
    printf("    FS_CAT </sd/path>                dump a file to console\n");
    printf("    SHIPMODE                         drop BATFET now (escape when button stuck)\n");
    printf("    REBOOT                           esp_restart() -- clean SW reset\n");
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
    float t_soc = esp_ts_read_c();
    vbat_adc_ensure_init();
    uint32_t v_adc = vbat_adc_read_mv();

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
    printf("  vbat_adc     = %lu mV   soc_temp = %.1f C\n",
           (unsigned long)v_adc, t_soc);
    printf("  bq_v (fake!) = %.3f V   pct = %u  charging = %d  pg = %d  fault = 0x%02X\n",
           bat.voltage, (unsigned)bat.percentage,
           bat.charging ? 1 : 0, bat.power_good ? 1 : 0, bat.fault);
    printf("  NVS: print_boot=%d  batt_test=%d  blackbox=%d  bb_cadence=%u s  rec_audio=%d\n",
           nvs_cfg_sys_get_print_on_boot() ? 1 : 0,
           nvs_cfg_sys_get_batt_test()     ? 1 : 0,
           nvs_cfg_sys_get_blackbox()      ? 1 : 0,
           (unsigned)nvs_cfg_sys_get_bb_cadence_s(),
           nvs_cfg_sys_get_rec_audio()     ? 1 : 0);
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

    float t_tmp = 0, t_bme = 0, t_lsm = 0, t_max = 0, t_soc = 0;
    float p_tmp = 999, p_bme = 999, p_lsm = 999, p_max = 999, p_soc = 999;
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
        t_soc = esp_ts_read_c();

        #define VALID_C(v) ((v) > -40.0f && (v) < 120.0f && (v) != 0.0f)
        bool all_valid = VALID_C(t_tmp) && VALID_C(t_bme) &&
                         VALID_C(t_lsm) && VALID_C(t_max) && VALID_C(t_soc);
        #undef VALID_C

        bool all_stable =
            fabsf(t_tmp - p_tmp) < STABLE_DELTA_C &&
            fabsf(t_bme - p_bme) < STABLE_DELTA_C &&
            fabsf(t_lsm - p_lsm) < STABLE_DELTA_C &&
            fabsf(t_max - p_max) < STABLE_DELTA_C &&
            fabsf(t_soc - p_soc) < STABLE_DELTA_C;

        if (all_valid && all_stable) {
            if (++hits >= STABLE_HITS) { timed_out = false; break; }
        } else {
            hits = 0;
        }

        p_tmp = t_tmp; p_bme = t_bme; p_lsm = t_lsm;
        p_max = t_max; p_soc = t_soc;

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
    printf("  soc  (ESP32-S3)    = %6.2f C\n", t_soc);
    printf("  bq   (BQ25619)     =    --   (TS network not populated on iv7.1)\n");

    if (!had_env)  broker_env_set_enabled(false);
    if (!had_imu)  broker_imu_set_enabled(false);
    if (!had_skin) broker_skin_set_enabled(false);
}

// ── RTC_DUMP + filesystem ───────────────────────────────────────────────────
static void rtc_cli_dump_pcf_regs(void) {
    uint8_t regs[18] = {0};
    esp_err_t r = pcf85063_read_regs_raw(I2C_NUM_0, 0x00, regs, sizeof(regs));
    if (r != ESP_OK) {
        printf("[RTC_DUMP] i2c read failed: %s\n", esp_err_to_name(r));
        return;
    }
    printf("[RTC_DUMP] PCF85063A registers 0x00..0x11:\n");
    static const char *names[18] = {
        "Control_1",   "Control_2",   "Offset",     "RAM_byte",
        "Seconds",     "Minutes",     "Hours",      "Days",
        "Weekdays",    "Months",      "Years",
        "Sec_alarm",   "Min_alarm",   "Hour_alarm", "Day_alarm", "Wday_alarm",
        "Timer_val",   "Timer_mode",
    };
    for (int i = 0; i < 18; i++) {
        printf("  0x%02X  %-11s = 0x%02X  (%3u)\n", i, names[i], regs[i], regs[i]);
    }
    if (regs[4] & 0x80) {
        printf("  ⚠ Seconds bit 7 (OS) = 1 -- oscillator was stopped, time INVALID\n");
    }
}

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
        esp_err_t r = pcf85063_sync_utc(I2C_NUM_0,
                                        (uint8_t)hr, (uint8_t)mi, (uint8_t)se,
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
    if (startswith_ci(line, "RTC_DUMP"))  { rtc_cli_dump_pcf_regs(); return; }
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
    if (startswith_ci(line, "RGB")) {
        const char *arg = line + 3;
        while (*arg == ' ' || *arg == '\t') arg++;
        if (startswith_ci(arg, "AUTO") || startswith_ci(arg, "RESET")) {
            ws2812_set_state(ws2812_get_state());
            printf("[RGB] manual override cleared -- firmware animations resume\n");
            return;
        }
        int r=-1, g=-1, b=-1;
        if (sscanf(arg, "%d %d %d", &r, &g, &b) == 3 &&
            r >= 0 && r <= 255 && g >= 0 && g <= 255 && b >= 0 && b <= 255) {
            ws2812_set_color((uint8_t)r, (uint8_t)g, (uint8_t)b);
            printf("[RGB] set to (%d, %d, %d) -- manual override latched; RGB AUTO to release\n",
                   r, g, b);
        } else {
            printf("[RGB] usage: RGB <r> <g> <b>   or   RGB AUTO   (each 0..255)\n");
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
