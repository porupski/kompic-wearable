/**
 * @file nvs_cfg.c
 * @brief Per-driver NVS command state -- see nvs_cfg.h.
 */

#include "nvs_cfg.h"

#include <string.h>
#include <inttypes.h>
#include <stdio.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "pcf85063.h"

static const char *TAG = "NVS_CFG";

#define NS_RTC  "cfg_rtc"
#define NS_SYS  "cfg_sys"

// -- Keys (NVS keys are max 15 chars) ------------------------------------------
#define K_RTC_WALL_TS       "wall_ts"
#define K_RTC_WR_MS         "wr_ms"
#define K_RTC_BOOT_SEQ      "boot_seq"
#define K_RTC_LAST_SET_TIME "last_settime"    // 0.4.12: was K_RTC_LAST_CMD
#define K_RTC_LAST_CMD_OLD  "last_cmd"        // backward-compat read only

#define K_SYS_PRINT     "print_boot"  // u8; 0 = quiet, 1 = print (default)
#define K_SYS_BATT_TEST "batt_test"   // u8; 0 = normal, 1 = battery-test mode
#define K_SYS_BLACKBOX  "blackbox"    // u8; 0 = off, 1 = BLACKBOX telemetry
#define K_SYS_BB_CAD    "bb_cadence"  // u16; sample cadence in seconds (default 10)
#define K_SYS_REC_AUDIO "rec_audio"   // u8; 0 = skip mic annot, 1 = 5 s pre-roll (default)
#define K_SYS_LAST_FW   "last_fw"     // str; MAJOR.MINOR.PATCH of last-seen fw
#define K_SYS_PC_SYNC_OFF "pc_sync_off_us"  // i64; signed ECG offset µs
#define K_SYS_PC_SYNC_UPT "pc_sync_upt_us"  // i64; esp_timer at write (staleness)

// ── RTC ────────────────────────────────────────────────────────────────────────

esp_err_t nvs_cfg_rtc_load(nvs_cfg_rtc_t *out)
{
    if (out == NULL) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));

    nvs_handle_t h;
    esp_err_t err = nvs_open(NS_RTC, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        out->valid = false;
        return ESP_OK;
    }
    if (err != ESP_OK) return err;

    // If any key is present, mark the record valid. Missing keys keep their
    // zero defaults.
    bool any = false;

    uint64_t u64 = 0;
    if (nvs_get_u64(h, K_RTC_WALL_TS, &u64) == ESP_OK) { out->wall_ts = u64; any = true; }
    if (nvs_get_u64(h, K_RTC_WR_MS,   &u64) == ESP_OK) { out->wr_ms   = u64; any = true; }

    uint32_t u32 = 0;
    if (nvs_get_u32(h, K_RTC_BOOT_SEQ, &u32) == ESP_OK) { out->boot_seq = u32; any = true; }

    size_t sz = sizeof(out->last_set_time);
    // Prefer the new key; fall back to the legacy K_RTC_LAST_CMD for records
    // written by fw <= 0.4.11 so a firmware upgrade doesn't lose history.
    if (nvs_get_str(h, K_RTC_LAST_SET_TIME, out->last_set_time, &sz) == ESP_OK) {
        any = true;
    } else {
        sz = sizeof(out->last_set_time);
        if (nvs_get_str(h, K_RTC_LAST_CMD_OLD, out->last_set_time, &sz) == ESP_OK) {
            any = true;
        } else {
            out->last_set_time[0] = 0;
        }
    }

    out->valid = any;
    nvs_close(h);
    return ESP_OK;
}

esp_err_t nvs_cfg_rtc_save(uint64_t wall_ts, uint64_t wr_ms,
                            uint32_t boot_seq, const char *last_set_time)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS_RTC, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;

    err  = nvs_set_u64(h, K_RTC_WALL_TS,  wall_ts);
    err |= nvs_set_u64(h, K_RTC_WR_MS,    wr_ms);
    err |= nvs_set_u32(h, K_RTC_BOOT_SEQ, boot_seq);

    // Sanitise: reject NULL, cap at NVS_CFG_LAST_CMD_MAX-1. Writes only to
    // the new key; the legacy K_RTC_LAST_CMD_OLD is read-only for backward
    // compat with fw <= 0.4.11 records.
    char buf[NVS_CFG_LAST_CMD_MAX];
    if (last_set_time == NULL) {
        buf[0] = 0;
    } else {
        strncpy(buf, last_set_time, sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = 0;
    }
    err |= nvs_set_str(h, K_RTC_LAST_SET_TIME, buf);

    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "rtc_save failed: %s", esp_err_to_name(err));
    }
    return err;
}

// ── System ─────────────────────────────────────────────────────────────────────

bool nvs_cfg_sys_get_print_on_boot(void)
{
    nvs_handle_t h;
    if (nvs_open(NS_SYS, NVS_READONLY, &h) != ESP_OK) return true;  // default: print
    uint8_t v = 1;
    nvs_get_u8(h, K_SYS_PRINT, &v);
    nvs_close(h);
    return v != 0;
}

esp_err_t nvs_cfg_sys_set_print_on_boot(bool enabled)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS_SYS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_u8(h, K_SYS_PRINT, enabled ? 1 : 0);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

bool nvs_cfg_sys_get_batt_test(void)
{
    nvs_handle_t h;
    if (nvs_open(NS_SYS, NVS_READONLY, &h) != ESP_OK) return false;
    uint8_t v = 0;
    nvs_get_u8(h, K_SYS_BATT_TEST, &v);
    nvs_close(h);
    return v != 0;
}

esp_err_t nvs_cfg_sys_set_batt_test(bool enabled)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS_SYS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_u8(h, K_SYS_BATT_TEST, enabled ? 1 : 0);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

bool nvs_cfg_sys_get_blackbox(void)
{
    nvs_handle_t h;
    if (nvs_open(NS_SYS, NVS_READONLY, &h) != ESP_OK) return false;
    uint8_t v = 0;
    nvs_get_u8(h, K_SYS_BLACKBOX, &v);
    nvs_close(h);
    return v != 0;
}

esp_err_t nvs_cfg_sys_set_blackbox(bool enabled)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS_SYS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_u8(h, K_SYS_BLACKBOX, enabled ? 1 : 0);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

uint16_t nvs_cfg_sys_get_bb_cadence_s(void)
{
    nvs_handle_t h;
    if (nvs_open(NS_SYS, NVS_READONLY, &h) != ESP_OK) return 10;
    uint16_t v = 10;
    nvs_get_u16(h, K_SYS_BB_CAD, &v);
    nvs_close(h);
    if (v < 1)     v = 1;
    if (v > 3600)  v = 3600;
    return v;
}

esp_err_t nvs_cfg_sys_set_bb_cadence_s(uint16_t s)
{
    if (s < 1)     s = 1;
    if (s > 3600)  s = 3600;
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS_SYS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_u16(h, K_SYS_BB_CAD, s);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

bool nvs_cfg_sys_get_rec_audio(void)
{
    // Default TRUE -- preserves the historical 5 s mic annotation pre-roll
    // for anyone who never touches this flag. Toggle via "REC_AUDIO OFF".
    nvs_handle_t h;
    if (nvs_open(NS_SYS, NVS_READONLY, &h) != ESP_OK) return true;
    uint8_t v = 1;
    nvs_get_u8(h, K_SYS_REC_AUDIO, &v);
    nvs_close(h);
    return v != 0;
}

esp_err_t nvs_cfg_sys_set_rec_audio(bool enabled)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS_SYS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_u8(h, K_SYS_REC_AUDIO, enabled ? 1 : 0);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

esp_err_t nvs_cfg_sys_get_last_fw(char *out, size_t out_len)
{
    if (out == NULL || out_len == 0) return ESP_ERR_INVALID_ARG;
    out[0] = 0;
    nvs_handle_t h;
    if (nvs_open(NS_SYS, NVS_READONLY, &h) != ESP_OK) return ESP_OK;
    size_t sz = out_len;
    (void)nvs_get_str(h, K_SYS_LAST_FW, out, &sz);   // silently OK on NOT_FOUND
    nvs_close(h);
    return ESP_OK;
}

esp_err_t nvs_cfg_sys_set_last_fw(const char *fw)
{
    if (fw == NULL) return ESP_ERR_INVALID_ARG;
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS_SYS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_str(h, K_SYS_LAST_FW, fw);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

void nvs_cfg_sys_check_fw_version(const char *current_fw)
{
    if (current_fw == NULL) return;
    char stored[NVS_CFG_FW_STR_MAX] = {0};
    (void)nvs_cfg_sys_get_last_fw(stored, sizeof(stored));

    if (stored[0] == 0) {
        ESP_LOGI(TAG, "fw version first-seen: %s (writing to NVS)", current_fw);
        (void)nvs_cfg_sys_set_last_fw(current_fw);
    } else if (strcmp(stored, current_fw) != 0) {
        ESP_LOGW(TAG, "fw version UPGRADED: %s -> %s (updating NVS)",
                 stored, current_fw);
        (void)nvs_cfg_sys_set_last_fw(current_fw);
    } else {
        ESP_LOGI(TAG, "fw version unchanged since last boot: %s", current_fw);
    }
}

// ── PC sync ────────────────────────────────────────────────────────────────────

esp_err_t nvs_cfg_sys_get_pc_sync(int64_t *offset_us, int64_t *write_uptime_us)
{
    if (!offset_us || !write_uptime_us) return ESP_ERR_INVALID_ARG;
    *offset_us      = 0;
    *write_uptime_us = 0;
    nvs_handle_t h;
    if (nvs_open(NS_SYS, NVS_READONLY, &h) != ESP_OK) return ESP_ERR_NVS_NOT_FOUND;
    esp_err_t r1 = nvs_get_i64(h, K_SYS_PC_SYNC_OFF, offset_us);
    esp_err_t r2 = nvs_get_i64(h, K_SYS_PC_SYNC_UPT, write_uptime_us);
    nvs_close(h);
    return (r1 == ESP_OK && r2 == ESP_OK) ? ESP_OK : ESP_ERR_NVS_NOT_FOUND;
}

esp_err_t nvs_cfg_sys_set_pc_sync(int64_t offset_us, int64_t write_uptime_us)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS_SYS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err  = nvs_set_i64(h, K_SYS_PC_SYNC_OFF, offset_us);
    err |= nvs_set_i64(h, K_SYS_PC_SYNC_UPT, write_uptime_us);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) ESP_LOGE(TAG, "pc_sync save failed: %s", esp_err_to_name(err));
    return err;
}

// ── Boot printout ──────────────────────────────────────────────────────────────

static void print_unix_utc(uint64_t ts, char *out, size_t n)
{
    // Cheap Unix -> ISO conversion (avoid pulling in gmtime for one line).
    // Only accurate for 1970..2099 which is our operational window.
    // Days per month (Jan..Dec) for non-leap year.
    static const uint8_t dpm[] = {31,28,31,30,31,30,31,31,30,31,30,31};

    uint64_t s = ts % 60;  ts /= 60;
    uint64_t m = ts % 60;  ts /= 60;
    uint64_t h = ts % 24;  ts /= 24;
    // ts is now days since 1970-01-01
    uint32_t year = 1970;
    for (;;) {
        uint32_t dy = ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0) ? 366 : 365;
        if (ts < dy) break;
        ts -= dy;
        year++;
    }
    uint32_t mo = 0;
    for (mo = 0; mo < 12; mo++) {
        uint32_t dm = dpm[mo];
        if (mo == 1 && ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0)) dm = 29;
        if (ts < dm) break;
        ts -= dm;
    }
    uint32_t day = (uint32_t)ts + 1;

    snprintf(out, n, "%04u-%02u-%02uT%02u:%02u:%02uZ",
             (unsigned)year, (unsigned)(mo + 1), (unsigned)day,
             (unsigned)h, (unsigned)m, (unsigned)s);
}

static const char *cmd_name(uint8_t v)
{
    switch (v) {
    case PCF85063_RAM_CMD_NONE:     return "NONE";
    case PCF85063_RAM_CMD_SET_TIME: return "SET_TIME";
    case PCF85063_RAM_CMD_GET_TIME: return "GET_TIME";
    default:                        return "unknown";
    }
}

void nvs_cfg_boot_print(int i2c_num)
{
    if (!nvs_cfg_sys_get_print_on_boot()) {
        ESP_LOGI(TAG, "boot printout disabled (cfg_sys.print_boot=0)");
        return;
    }

    nvs_cfg_rtc_t r;
    esp_err_t err = nvs_cfg_rtc_load(&r);

    printf("\n");
    printf("[NVS] ── cfg_rtc ──\n");
    if (err != ESP_OK) {
        printf("[NVS]   load failed: %s\n", esp_err_to_name(err));
    } else if (!r.valid) {
        printf("[NVS]   (empty -- SET_TIME has not been called since NVS erase)\n");
    } else {
        char iso[32];
        print_unix_utc(r.wall_ts, iso, sizeof(iso));
        printf("[NVS]   wall_ts        = %" PRIu64 "  (%s)\n", r.wall_ts, iso);
        printf("[NVS]   wr_ms          = %" PRIu64 "\n", r.wr_ms);
        printf("[NVS]   boot_seq       = %" PRIu32 "\n", r.boot_seq);
        printf("[NVS]   last_set_time  = \"%s\"\n", r.last_set_time);
    }

    uint8_t ram = 0;
    if (pcf85063_ram_byte_read(i2c_num, &ram) == ESP_OK) {
        printf("[PCF]   RAM_byte  = 0x%02X  (%s)\n", ram, cmd_name(ram));
    } else {
        printf("[PCF]   RAM_byte  = ?? (I2C read failed)\n");
    }
    printf("[SYS]   print_boot= 1  (toggle: NVS_PRINT OFF)\n");
    printf("[SYS]   batt_test = %d  (toggle: BATT_TEST ON|OFF -- reboot to apply)\n"
           "                       Note: VBUS-detected at boot auto-skips batt_test (recovery)\n",
           nvs_cfg_sys_get_batt_test() ? 1 : 0);
    printf("[SYS]   blackbox  = %d  cadence=%u s  (BLACKBOX ON|OFF, BLACKBOX_CADENCE <s>)\n",
           nvs_cfg_sys_get_blackbox() ? 1 : 0,
           (unsigned)nvs_cfg_sys_get_bb_cadence_s());
    printf("[SYS]   rec_audio = %d  (toggle: REC_AUDIO ON|OFF -- skip 5 s mic annotation)\n",
           nvs_cfg_sys_get_rec_audio() ? 1 : 0);
    {
        char last_fw[NVS_CFG_FW_STR_MAX] = {0};
        (void)nvs_cfg_sys_get_last_fw(last_fw, sizeof(last_fw));
        printf("[SYS]   last_fw   = \"%s\"  (updated by nvs_cfg_sys_check_fw_version at boot)\n",
               last_fw[0] ? last_fw : "(none)");
    }
    {
        int64_t off_us = 0, upt_us = 0;
        if (nvs_cfg_sys_get_pc_sync(&off_us, &upt_us) == ESP_OK) {
            printf("[SYS]   pc_sync   = offset=%" PRId64 " µs  written_uptime=%" PRId64 " µs\n",
                   off_us, upt_us);
        } else {
            printf("[SYS]   pc_sync   = (not stored)\n");
        }
    }
    printf("\n");
}
