/**
 * @file lsm6dsv16x_emb.c
 * @brief LSM6DSV16X embedded-functions driver -- pedometer, activity/inactivity,
 *        tap-Z, sleep-state, MLC-output register plumbing.
 *
 * All embedded-function register writes go through the bank-switch helpers in
 * this file. The rule (WARN-01 in the 20.10b Advanced Features extract):
 * every enter must be paired with an exit, or subsequent CTRL* writes silently
 * land in the embedded bank instead. The helpers here handle that discipline.
 *
 * All public entry points take and release g_i2c_mutex internally. Do not call
 * them while already holding the mutex. Safe from any Core 0 task context.
 *
 * See lsm6dsv16x.h for the API contract and register/bit definitions.
 */

#include "lsm6dsv16x.h"
#include "driver/i2c.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

extern SemaphoreHandle_t g_i2c_mutex;

static const char *TAG = "LSM_EMB";

// I2C timeout for a single transaction. Matches the value used in lsm6dsv16x.c
// so behavior on bus contention is uniform across the driver.
#define I2C_TIMEOUT_MS  20

// ---------------------------------------------------------------------------
// 16-bit -> 32-bit pedometer widening state
//   The chip counter is a 16-bit register that wraps at 65536. We track the
//   last raw16 value read and accumulate deltas into a 32-bit host counter.
//   Rollover is detected as (now16 < last16) after subtracting the wrap.
//   Reset (pedometer_reset() or first read after enable) zeros the accumulator
//   and re-seeds last16 to whatever the chip currently reports.
// ---------------------------------------------------------------------------
static uint16_t s_pedo_last16     = 0;
static uint32_t s_pedo_total      = 0;
static bool     s_pedo_seeded     = false;

// ---------------------------------------------------------------------------
// I2C primitives -- caller holds g_i2c_mutex. Mirror the shape of the ones
// in lsm6dsv16x.c but kept file-local to avoid cross-file linkage churn.
// ---------------------------------------------------------------------------
static esp_err_t write_reg_locked(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    return i2c_master_write_to_device(I2C_NUM_0, LSM6DSV16X_I2C_ADDR,
                                      buf, 2, pdMS_TO_TICKS(I2C_TIMEOUT_MS));
}

static esp_err_t read_reg_locked(uint8_t reg, uint8_t *out)
{
    return i2c_master_write_read_device(I2C_NUM_0, LSM6DSV16X_I2C_ADDR,
                                        &reg, 1, out, 1,
                                        pdMS_TO_TICKS(I2C_TIMEOUT_MS));
}

static esp_err_t read_buf_locked(uint8_t reg, uint8_t *out, size_t n)
{
    return i2c_master_write_read_device(I2C_NUM_0, LSM6DSV16X_I2C_ADDR,
                                        &reg, 1, out, n,
                                        pdMS_TO_TICKS(I2C_TIMEOUT_MS));
}

// ---------------------------------------------------------------------------
// Bank-switch primitives (caller holds g_i2c_mutex, does NOT release between)
//   Pair every _enter_locked with an _exit_locked before returning. If any
//   embedded-bank write fails, still call _exit_locked so the bank state
//   returns to main -- otherwise subsequent CTRL* writes go to the wrong bank.
// ---------------------------------------------------------------------------
static esp_err_t bank_enter_locked(void)
{
    return write_reg_locked(REG_FUNC_CFG_ACCESS, FUNC_CFG_EMB_ACCESS);
}

static esp_err_t bank_exit_locked(void)
{
    return write_reg_locked(REG_FUNC_CFG_ACCESS, 0x00);
}

// ---------------------------------------------------------------------------
// Public bank helpers -- expose the primitive for the MLC .ucf loader, which
// needs to hold the bank open across many writes. Caller manages the mutex.
// ---------------------------------------------------------------------------
esp_err_t lsm6dsv16x_emb_bank_enter(void)
{
    if (xSemaphoreTake(g_i2c_mutex, pdMS_TO_TICKS(50)) != pdTRUE) return ESP_ERR_TIMEOUT;
    esp_err_t r = bank_enter_locked();
    xSemaphoreGive(g_i2c_mutex);
    return r;
}

esp_err_t lsm6dsv16x_emb_bank_exit(void)
{
    if (xSemaphoreTake(g_i2c_mutex, pdMS_TO_TICKS(50)) != pdTRUE) return ESP_ERR_TIMEOUT;
    esp_err_t r = bank_exit_locked();
    xSemaphoreGive(g_i2c_mutex);
    return r;
}

// ---------------------------------------------------------------------------
// Pedometer
// ---------------------------------------------------------------------------
esp_err_t lsm6dsv16x_pedometer_enable(bool en)
{
    if (xSemaphoreTake(g_i2c_mutex, pdMS_TO_TICKS(50)) != pdTRUE) return ESP_ERR_TIMEOUT;

    esp_err_t r = bank_enter_locked();
    if (r == ESP_OK) {
        uint8_t v = 0;
        (void)read_reg_locked(REG_EMB_EMB_FUNC_EN_A, &v);
        if (en) v |=  EMB_FUNC_EN_A_PEDO_EN;
        else    v &= ~EMB_FUNC_EN_A_PEDO_EN;
        r = write_reg_locked(REG_EMB_EMB_FUNC_EN_A, v);
    }
    esp_err_t rx = bank_exit_locked();
    xSemaphoreGive(g_i2c_mutex);
    if (r == ESP_OK) r = rx;

    // Reset widening state on any state change so a stale s_pedo_last16 from
    // a prior enable/disable cycle doesn't contribute a bogus delta.
    s_pedo_seeded = false;
    s_pedo_last16 = 0;
    s_pedo_total  = 0;

    ESP_LOGI(TAG, "pedometer %s (%s)", en ? "ENABLE" : "DISABLE",
             r == ESP_OK ? "ok" : esp_err_to_name(r));
    return r;
}

esp_err_t lsm6dsv16x_pedometer_reset(void)
{
    if (xSemaphoreTake(g_i2c_mutex, pdMS_TO_TICKS(50)) != pdTRUE) return ESP_ERR_TIMEOUT;

    esp_err_t r = bank_enter_locked();
    if (r == ESP_OK) {
        // Write 1 to PEDO_RST_STEP bit; it auto-clears. Existing bits in
        // EMB_FUNC_SRC are status-only, so a plain write is safe.
        r = write_reg_locked(REG_EMB_EMB_FUNC_SRC, EMB_FUNC_SRC_PEDO_RST);
    }
    esp_err_t rx = bank_exit_locked();
    xSemaphoreGive(g_i2c_mutex);
    if (r == ESP_OK) r = rx;

    s_pedo_seeded = false;
    s_pedo_last16 = 0;
    s_pedo_total  = 0;
    return r;
}

esp_err_t lsm6dsv16x_pedometer_read(uint32_t *steps_out)
{
    if (!steps_out) return ESP_ERR_INVALID_ARG;

    if (xSemaphoreTake(g_i2c_mutex, pdMS_TO_TICKS(50)) != pdTRUE) return ESP_ERR_TIMEOUT;

    uint8_t buf[2] = {0};
    esp_err_t r = bank_enter_locked();
    if (r == ESP_OK) {
        r = read_buf_locked(REG_EMB_STEP_COUNTER_L, buf, 2);
    }
    esp_err_t rx = bank_exit_locked();
    xSemaphoreGive(g_i2c_mutex);
    if (r != ESP_OK) return r;
    if (rx != ESP_OK) return rx;

    uint16_t now16 = (uint16_t)buf[0] | ((uint16_t)buf[1] << 8);
    if (!s_pedo_seeded) {
        s_pedo_last16 = now16;
        s_pedo_seeded = true;
    } else if (now16 < s_pedo_last16) {
        // Wrap: (0x10000 - last16) + now16
        s_pedo_total += (uint32_t)(0x10000u - s_pedo_last16) + now16;
    } else {
        s_pedo_total += (uint32_t)(now16 - s_pedo_last16);
    }
    s_pedo_last16 = now16;
    *steps_out = s_pedo_total;
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// Activity / inactivity auto-sleep
// ---------------------------------------------------------------------------
esp_err_t lsm6dsv16x_activity_sleep_enable(bool en)
{
    if (xSemaphoreTake(g_i2c_mutex, pdMS_TO_TICKS(50)) != pdTRUE) return ESP_ERR_TIMEOUT;

    // Configure timing/thresholds regardless of enable/disable so a later
    // enable finds them already set.
    (void)write_reg_locked(REG_INACTIVITY_DUR,
                           INACT_DUR_WEIGHT_62MG5 | INACT_DUR_ODR_15HZ | INACT_DUR_WAKE_2EVENTS);
    (void)write_reg_locked(REG_INACTIVITY_THS, 4);  // 4 * 62.5 mg = 250 mg

    uint8_t v = 0;
    (void)read_reg_locked(REG_FUNCTIONS_ENABLE, &v);
    v &= ~(0x03 << 2);  // clear INACT_EN[3:2]
    if (en) v |= FUNC_ENABLE_INACT_LP_GYRO_SLEEP;
    // Belt-and-suspenders: INTERRUPTS_ENABLE (bit 7) also gates the sleep-
    // transition event we want to observe. tap_z_enable() also sets it, but
    // set it here too so activity/inactivity works even if tap isn't in use.
    v |= (1 << 7);
    esp_err_t r = write_reg_locked(REG_FUNCTIONS_ENABLE, v);

    xSemaphoreGive(g_i2c_mutex);
    ESP_LOGI(TAG, "activity_sleep %s (%s)", en ? "ENABLE" : "DISABLE",
             r == ESP_OK ? "ok" : esp_err_to_name(r));
    return r;
}

bool lsm6dsv16x_is_sleeping(void)
{
    if (xSemaphoreTake(g_i2c_mutex, pdMS_TO_TICKS(20)) != pdTRUE) return false;
    uint8_t src = 0;
    esp_err_t r = read_reg_locked(REG_WAKE_UP_SRC, &src);
    xSemaphoreGive(g_i2c_mutex);
    if (r != ESP_OK) return false;
    return (src & WAKE_UP_SRC_SLEEP_STATE) != 0;
}

// ---------------------------------------------------------------------------
// Tap-Z (single + double)
// ---------------------------------------------------------------------------
esp_err_t lsm6dsv16x_tap_z_enable(bool en)
{
    if (xSemaphoreTake(g_i2c_mutex, pdMS_TO_TICKS(50)) != pdTRUE) return ESP_ERR_TIMEOUT;

    if (en) {
        // Config matched to sketch 12 findings (2026-07-23):
        //  * All three axes enabled -- wrist geometry tilts the chip Z axis
        //    ~60 deg off vertical, so a case tap lands mostly on X.
        //  * LIR=1 latches TAP_SRC until read -- critical for reliable polled
        //    operation. Without it, DOUBLE_TAP pulses can slip between the
        //    task_imu_fn 20 ms polls (bench-observed).
        //  * DUR=5 (~667 ms @ 240 Hz) -- shorter than the sketch's original
        //    DUR=6 so a single tap commits sooner; Ivan-verified.
        (void)write_reg_locked(REG_TAP_CFG0,
                               TAP_CFG0_TAP_X_EN | TAP_CFG0_TAP_Y_EN | TAP_CFG0_TAP_Z_EN |
                               TAP_CFG0_LIR);
        (void)write_reg_locked(REG_TAP_CFG1, 4);   // X threshold 500 mg @ +/-4g
        (void)write_reg_locked(REG_TAP_CFG2, 3);   // Y threshold 375 mg
        (void)write_reg_locked(REG_TAP_THS_6D, 3); // Z threshold 375 mg
        (void)write_reg_locked(REG_TAP_DUR, (5 << 4));   // DUR=5
        (void)write_reg_locked(REG_WAKE_UP_THS, WAKE_UP_THS_SINGLE_DOUBLE_TAP);
        // FUNCTIONS_ENABLE.INTERRUPTS_ENABLE (bit 7) gates the entire basic-
        // interrupts pipeline. REQUIRED for TAP_SRC to populate.
        uint8_t fe = 0;
        (void)read_reg_locked(REG_FUNCTIONS_ENABLE, &fe);
        fe |= (1 << 7);
        (void)write_reg_locked(REG_FUNCTIONS_ENABLE, fe);
    } else {
        (void)write_reg_locked(REG_TAP_CFG0, 0x00);
    }

    xSemaphoreGive(g_i2c_mutex);
    ESP_LOGI(TAG, "tap %s (X/Y/Z=4/3/3, DUR=5, LIR=1, ints_en=1)",
             en ? "ENABLE" : "DISABLE");
    return ESP_OK;
}

uint8_t lsm6dsv16x_tap_poll_src(void)
{
    if (xSemaphoreTake(g_i2c_mutex, pdMS_TO_TICKS(10)) != pdTRUE) return 0;
    uint8_t src = 0;
    esp_err_t r = read_reg_locked(REG_TAP_SRC, &src);
    xSemaphoreGive(g_i2c_mutex);
    if (r != ESP_OK) return 0;
    if (!(src & TAP_SRC_TAP_IA)) return 0;
    return src;
}

// ---------------------------------------------------------------------------
// MLC output. Empty until a .ucf classifier is loaded (see lsm6dsv16x_mlc.c
// once that lands). Wiring up now so the pipeline is testable end-to-end.
// ---------------------------------------------------------------------------
uint8_t lsm6dsv16x_mlc1_read(void)
{
    if (xSemaphoreTake(g_i2c_mutex, pdMS_TO_TICKS(20)) != pdTRUE) return 0;
    uint8_t v = 0;
    esp_err_t r = bank_enter_locked();
    if (r == ESP_OK) {
        (void)read_reg_locked(REG_EMB_MLC1_SRC, &v);
    }
    (void)bank_exit_locked();
    xSemaphoreGive(g_i2c_mutex);
    return v;
}

// ---------------------------------------------------------------------------
// .ucf loader -- parses ST's Unico / MEMS Studio classifier export text.
//
// Line grammar (only meaningful lines are "Ac ADDR VAL"; everything else is
// treated as a comment). Two hex bytes, whitespace-separated:
//   Ac 01 80        <- write 0x80 to register 0x01
//   Ac 04 08        <- write 0x08 to register 0x04
//   -- header       <- ignored
//
// The .ucf itself owns the bank-switch protocol -- the first Ac write is
// usually 01 80 (enter embedded bank) and the last is 01 00 (exit). We do
// not wrap it; we just apply writes in order.
//
// Robustness notes:
//   * The parser is permissive: whitespace ignored, comments ignored,
//     lines that don't match "Ac HH HH" are silently skipped.
//   * Any i2c write failure aborts and returns the error -- partial loads
//     are worse than none. Caller may retry.
//   * Not called during boot -- called on-demand when a user loads a
//     classifier via a future UI path (or from a test hook). Assumes the
//     chip is otherwise idle.
// ---------------------------------------------------------------------------
static bool parse_hex_byte(const char *p, uint8_t *out)
{
    uint8_t v = 0;
    for (int i = 0; i < 2; i++) {
        char c = p[i];
        uint8_t nib;
        if      (c >= '0' && c <= '9') nib = c - '0';
        else if (c >= 'a' && c <= 'f') nib = 10 + (c - 'a');
        else if (c >= 'A' && c <= 'F') nib = 10 + (c - 'A');
        else return false;
        v = (v << 4) | nib;
    }
    *out = v;
    return true;
}

esp_err_t lsm6dsv16x_mlc_load_ucf(const char *ucf_text, size_t len)
{
    if (!ucf_text || len == 0) return ESP_ERR_INVALID_ARG;

    if (xSemaphoreTake(g_i2c_mutex, pdMS_TO_TICKS(200)) != pdTRUE) return ESP_ERR_TIMEOUT;

    esp_err_t r = ESP_OK;
    size_t writes = 0;
    const char *p = ucf_text;
    const char *end = ucf_text + len;

    while (p < end) {
        // Skip leading whitespace.
        while (p < end && (*p == ' ' || *p == '\t')) p++;
        if (p >= end) break;
        // If line starts with "Ac " (address-write directive) parse it.
        if (p + 8 <= end && (p[0] == 'A' || p[0] == 'a') && (p[1] == 'c' || p[1] == 'C') && p[2] == ' ') {
            uint8_t addr = 0, val = 0;
            const char *a = p + 3;
            while (a < end && (*a == ' ' || *a == '\t')) a++;
            if (a + 2 > end || !parse_hex_byte(a, &addr)) goto next_line;
            const char *v = a + 2;
            while (v < end && (*v == ' ' || *v == '\t')) v++;
            if (v + 2 > end || !parse_hex_byte(v, &val)) goto next_line;
            r = write_reg_locked(addr, val);
            if (r != ESP_OK) {
                ESP_LOGE(TAG, "ucf write @0x%02X=0x%02X failed: %s (after %u writes)",
                         addr, val, esp_err_to_name(r), (unsigned)writes);
                xSemaphoreGive(g_i2c_mutex);
                return r;
            }
            writes++;
        }
next_line:
        while (p < end && *p != '\n') p++;
        if (p < end) p++;  // consume '\n'
    }

    // Insurance: leave the bank in main state regardless of what the .ucf
    // did. A well-formed .ucf ends with Ac 01 00, but not all do.
    (void)bank_exit_locked();

    xSemaphoreGive(g_i2c_mutex);
    ESP_LOGI(TAG, "ucf loaded (%u writes)", (unsigned)writes);
    return ESP_OK;
}
