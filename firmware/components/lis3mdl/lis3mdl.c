/**
 * @file lis3mdl.c
 * @brief ST LIS3MDLTR driver -- Core 0 only.
 *
 * Replaces qmc5883p.c at the chip layer. Carry-forward (chip-independent):
 *   - Hard-iron min/max sweep calibration
 *   - Heading calc via atan2f
 *   - NVS-seeded calibration restore at boot
 *   - broker_mag_data_t integration (Blueprint 9 §2)
 *   - calibrating / cal_countdown UI feedback
 *
 * New: lis3mdl_measure_lra_offset() -- characterises the haptic LRA's static
 * field at the magnetometer. Per v7.2 line 432 we expect ~1 gauss; we measure
 * it once during first-boot calibration and subtract.
 */

#include "lis3mdl.h"
#include "data_broker.h"
#include "boot_hw_init.h"   // g_i2c_mutex
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <math.h>
#include <string.h>

static const char *TAG = "LIS3MDL";

// -- Internal calibration state -----------------------------------------------
static lis3mdl_calibration_t s_cal = {0};
static float s_min_x =  1e6f, s_max_x = -1e6f;
static float s_min_y =  1e6f, s_max_y = -1e6f;

// -- Identity -----------------------------------------------------------------
const char *lis3mdl_get_chip_name(void) { return "LIS3MDLTR";          }
const char *lis3mdl_get_chip_desc(void) { return "3-axis magnetometer"; }

// =============================================================================
// I2C helpers (caller holds g_i2c_mutex)
// =============================================================================

static esp_err_t write_reg(i2c_port_t port, uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    return i2c_master_write_to_device(port, LIS3MDL_ADDR,
                                      buf, 2, pdMS_TO_TICKS(20));
}

static esp_err_t read_reg(i2c_port_t port, uint8_t reg, uint8_t *val)
{
    return i2c_master_write_read_device(port, LIS3MDL_ADDR,
                                        &reg, 1, val, 1, pdMS_TO_TICKS(20));
}

static esp_err_t read_burst(i2c_port_t port, uint8_t reg, uint8_t *buf, size_t len)
{
    // LIS3MDL requires the auto-increment bit (0x80) set in the sub-address
    // for multi-byte reads.
    uint8_t addr = reg | LIS3MDL_AUTO_INC;
    return i2c_master_write_read_device(port, LIS3MDL_ADDR,
                                        &addr, 1, buf, len, pdMS_TO_TICKS(20));
}

// =============================================================================
// Init
// =============================================================================
esp_err_t lis3mdl_init(i2c_port_t i2c_num)
{
    if (xSemaphoreTake(g_i2c_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    uint8_t who = 0;
    esp_err_t ret = read_reg(i2c_num, LIS3MDL_REG_WHO_AM_I, &who);
    if (ret != ESP_OK || who != LIS3MDL_WHO_AM_I_VAL) {
        xSemaphoreGive(g_i2c_mutex);
        ESP_LOGE(TAG, "WHO_AM_I failed: got 0x%02X, expected 0x%02X",
                 who, LIS3MDL_WHO_AM_I_VAL);
        return ESP_FAIL;
    }

    // Soft reset via CTRL2.SOFT_RST
    (void)write_reg(i2c_num, LIS3MDL_REG_CTRL2, CTRL2_SOFT_RST);
    xSemaphoreGive(g_i2c_mutex);
    vTaskDelay(pdMS_TO_TICKS(10));

    if (xSemaphoreTake(g_i2c_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    // CTRL1: UHP X/Y, 10 Hz, no temp, no self-test
    (void)write_reg(i2c_num, LIS3MDL_REG_CTRL1, CTRL1_OM_UHP_XY | CTRL1_DO_10HZ);
    // CTRL2: ±4 G full-scale
    (void)write_reg(i2c_num, LIS3MDL_REG_CTRL2, CTRL2_FS_4G);
    // CTRL4: UHP Z (match X/Y mode)
    (void)write_reg(i2c_num, LIS3MDL_REG_CTRL4, CTRL4_OMZ_UHP);
    // CTRL5: BDU (block data update -- prevents tearing across burst reads)
    (void)write_reg(i2c_num, LIS3MDL_REG_CTRL5, CTRL5_BDU);
    // CTRL3: continuous-conversion mode (last -- starts measurements)
    (void)write_reg(i2c_num, LIS3MDL_REG_CTRL3, CTRL3_MD_CONTINUOUS);

    xSemaphoreGive(g_i2c_mutex);
    vTaskDelay(pdMS_TO_TICKS(20));

    ESP_LOGI(TAG, "%s init OK @ 0x%02X (WHO_AM_I=0x%02X)",
             lis3mdl_get_chip_name(), LIS3MDL_ADDR, who);
    return ESP_OK;
}

void lis3mdl_deinit(void)
{
    if (xSemaphoreTake(g_i2c_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        (void)write_reg(I2C_NUM_0, LIS3MDL_REG_CTRL3, CTRL3_MD_POWER_DOWN);
        xSemaphoreGive(g_i2c_mutex);
    }
    ESP_LOGI(TAG, "Powered down");
}

// =============================================================================
// Measurement (caller holds g_i2c_mutex)
// =============================================================================
esp_err_t lis3mdl_read_raw(i2c_port_t i2c_num, float *x_ut, float *y_ut, float *z_ut)
{
    if (!x_ut || !y_ut || !z_ut) return ESP_ERR_INVALID_ARG;

    uint8_t buf[6];
    esp_err_t ret = read_burst(i2c_num, LIS3MDL_REG_OUT_X_L, buf, sizeof(buf));
    if (ret != ESP_OK) return ret;

    int16_t rx = (int16_t)((buf[1] << 8) | buf[0]);
    int16_t ry = (int16_t)((buf[3] << 8) | buf[2]);
    int16_t rz = (int16_t)((buf[5] << 8) | buf[4]);

    *x_ut = (float)rx / LIS3MDL_LSB_PER_UT;
    *y_ut = (float)ry / LIS3MDL_LSB_PER_UT;
    *z_ut = (float)rz / LIS3MDL_LSB_PER_UT;
    return ESP_OK;
}

float lis3mdl_calculate_heading(float x_ut, float y_ut, float declination_deg)
{
    // Housing rotation correction is identity here (board axes match world XY).
    // If the LIS3MDL is rotated relative to the watch crown, adjust via swap/sign.
    float heading = atan2f(y_ut, x_ut) * (180.0f / (float)M_PI);
    heading += declination_deg;
    if (heading < 0.0f)   heading += 360.0f;
    if (heading >= 360.0f) heading -= 360.0f;
    return heading;
}

// =============================================================================
// Power management
// =============================================================================
esp_err_t lis3mdl_set_standby(i2c_port_t i2c_num)
{
    esp_err_t ret;
    if (xSemaphoreTake(g_i2c_mutex, pdMS_TO_TICKS(50)) != pdTRUE) return ESP_ERR_TIMEOUT;
    ret = write_reg(i2c_num, LIS3MDL_REG_CTRL3, CTRL3_MD_POWER_DOWN);
    xSemaphoreGive(g_i2c_mutex);
    return ret;
}

esp_err_t lis3mdl_wake(i2c_port_t i2c_num)
{
    esp_err_t ret;
    if (xSemaphoreTake(g_i2c_mutex, pdMS_TO_TICKS(50)) != pdTRUE) return ESP_ERR_TIMEOUT;
    ret = write_reg(i2c_num, LIS3MDL_REG_CTRL3, CTRL3_MD_CONTINUOUS);
    xSemaphoreGive(g_i2c_mutex);
    return ret;
}

// =============================================================================
// Calibration -- carries forward from qmc5883p.c (chip-independent)
// =============================================================================
void lis3mdl_seed_calibration(float offset_x, float offset_y,
                               float scale_x,  float scale_y,
                               float scale_avg)
{
    s_cal.offset_x   = offset_x;
    s_cal.offset_y   = offset_y;
    s_cal.scale_x    = scale_x;
    s_cal.scale_y    = scale_y;
    s_cal.scale_avg  = scale_avg;
    s_cal.calibrated = (scale_avg > 0.0f);
    ESP_LOGI(TAG, "Calibration seeded: off=(%.1f,%.1f) scale=(%.3f,%.3f) avg=%.3f cal=%d",
             (double)offset_x, (double)offset_y,
             (double)scale_x,  (double)scale_y,
             (double)scale_avg, (int)s_cal.calibrated);
}

void lis3mdl_start_calibration(lis3mdl_calibration_t *cal)
{
    s_min_x =  1e6f; s_max_x = -1e6f;
    s_min_y =  1e6f; s_max_y = -1e6f;
    if (cal) memset(cal, 0, sizeof(*cal));
    ESP_LOGI(TAG, "Calibration started -- rotate device in figure-8");
}

void lis3mdl_update_calibration(lis3mdl_calibration_t *cal, float x_ut, float y_ut)
{
    (void)cal;
    if (x_ut < s_min_x) s_min_x = x_ut;
    if (x_ut > s_max_x) s_max_x = x_ut;
    if (y_ut < s_min_y) s_min_y = y_ut;
    if (y_ut > s_max_y) s_max_y = y_ut;
}

esp_err_t lis3mdl_finish_calibration(lis3mdl_calibration_t *cal)
{
    if (!cal) return ESP_ERR_INVALID_ARG;
    float dx = (s_max_x - s_min_x);
    float dy = (s_max_y - s_min_y);
    if (dx < 5.0f || dy < 5.0f) {
        ESP_LOGW(TAG, "Cal range too small: dx=%.2f dy=%.2f -- rejecting", (double)dx, (double)dy);
        return ESP_FAIL;
    }

    cal->offset_x   = (s_max_x + s_min_x) * 0.5f;
    cal->offset_y   = (s_max_y + s_min_y) * 0.5f;
    cal->scale_x    = dx * 0.5f;
    cal->scale_y    = dy * 0.5f;
    cal->scale_avg  = (cal->scale_x + cal->scale_y) * 0.5f;
    cal->calibrated = true;
    memcpy(&s_cal, cal, sizeof(s_cal));

    ESP_LOGI(TAG, "Calibration finished: off=(%.1f,%.1f) scale=(%.1f,%.1f) avg=%.1f",
             (double)cal->offset_x, (double)cal->offset_y,
             (double)cal->scale_x,  (double)cal->scale_y,
             (double)cal->scale_avg);
    return ESP_OK;
}

esp_err_t lis3mdl_measure_lra_offset(i2c_port_t i2c_num,
                                      float *out_offset_x_ut,
                                      float *out_offset_y_ut,
                                      float *out_offset_z_ut)
{
    const int N = 20;     // 20 samples over ~1 second at 50 ms spacing
    float sum_x = 0.0f, sum_y = 0.0f, sum_z = 0.0f;
    int   got = 0;

    for (int i = 0; i < N; i++) {
        float x, y, z;
        if (xSemaphoreTake(g_i2c_mutex, pdMS_TO_TICKS(100)) != pdTRUE) continue;
        esp_err_t ret = lis3mdl_read_raw(i2c_num, &x, &y, &z);
        xSemaphoreGive(g_i2c_mutex);
        if (ret == ESP_OK) {
            sum_x += x; sum_y += y; sum_z += z;
            got++;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    if (got < N / 2) {
        ESP_LOGE(TAG, "LRA offset: only %d/%d samples succeeded", got, N);
        return ESP_FAIL;
    }

    if (out_offset_x_ut) *out_offset_x_ut = sum_x / (float)got;
    if (out_offset_y_ut) *out_offset_y_ut = sum_y / (float)got;
    if (out_offset_z_ut) *out_offset_z_ut = sum_z / (float)got;
    ESP_LOGI(TAG, "LRA offset (%d samples): X=%.2f Y=%.2f Z=%.2f uT",
             got, (double)(sum_x / got), (double)(sum_y / got), (double)(sum_z / got));
    return ESP_OK;
}

// =============================================================================
// Sensor task
// =============================================================================
static uint8_t heading_to_cardinal(float deg)
{
    if (deg < 22.5f || deg >= 337.5f) return 0;  // N
    if (deg <  67.5f) return 1;  // NE
    if (deg < 112.5f) return 2;  // E
    if (deg < 157.5f) return 3;  // SE
    if (deg < 202.5f) return 4;  // S
    if (deg < 247.5f) return 5;  // SW
    if (deg < 292.5f) return 6;  // W
    return 7;                    // NW
}

void task_mag_fn(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "Task started on Core %d", xPortGetCoreID());

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(LIS3MDL_POLL_MS));
        if (!broker_mag_hw_alive())    continue;
        if (!broker_mag_get_enabled()) continue;

        broker_mag_data_t bd = {0};
        broker_mag_read(&bd);

        float x, y, z;
        if (xSemaphoreTake(g_i2c_mutex, pdMS_TO_TICKS(50)) != pdTRUE) continue;
        esp_err_t ret = lis3mdl_read_raw(I2C_NUM_0, &x, &y, &z);
        xSemaphoreGive(g_i2c_mutex);
        if (ret != ESP_OK) continue;

        // Apply hard-iron offset (always) and scale normalisation (if calibrated).
        float cx = x - s_cal.offset_x;
        float cy = y - s_cal.offset_y;
        if (s_cal.calibrated && s_cal.scale_avg > 0.0f) {
            cx *= (s_cal.scale_avg / s_cal.scale_x);
            cy *= (s_cal.scale_avg / s_cal.scale_y);
        }

        bd.x_ut         = cx;
        bd.y_ut         = cy;
        bd.z_ut         = z;
        bd.heading_deg  = lis3mdl_calculate_heading(cx, cy, 0.0f);
        bd.cardinal     = heading_to_cardinal(bd.heading_deg);
        bd.calibrated   = s_cal.calibrated;
        // bd.calibrating / cal_countdown owned by task_mag_cal_fn
        broker_mag_write(&bd);
    }
}

// =============================================================================
// Calibration task -- figure-8 rotation, 30 s window
// =============================================================================
void task_mag_cal_fn(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "Calibration task started on Core %d", xPortGetCoreID());

    while (1) {
        // Wait for the UI to flip broker.calibrating = true.
        vTaskDelay(pdMS_TO_TICKS(200));
        broker_mag_data_t bd = {0};
        broker_mag_read(&bd);
        if (!bd.calibrating) continue;

        lis3mdl_calibration_t cal = {0};
        lis3mdl_start_calibration(&cal);

        const int total_s = 30;
        for (int s = total_s; s > 0; s--) {
            bd.cal_countdown = (uint8_t)s;
            broker_mag_write(&bd);

            int64_t t_end = esp_timer_get_time() + 1000000;
            while (esp_timer_get_time() < t_end) {
                broker_mag_data_t live = {0};
                broker_mag_read(&live);
                if (!live.calibrating) goto cancel;
                lis3mdl_update_calibration(&cal, live.x_ut, live.y_ut);
                vTaskDelay(pdMS_TO_TICKS(50));
            }
        }

        if (lis3mdl_finish_calibration(&cal) == ESP_OK) {
            broker_mag_read(&bd);
            bd.calibrated    = true;
            bd.calibrating   = false;
            bd.cal_countdown = 0;
            broker_mag_write(&bd);
        } else {
            broker_mag_read(&bd);
            bd.calibrating   = false;
            bd.cal_countdown = 0;
            broker_mag_write(&bd);
        }
        continue;

cancel:
        broker_mag_read(&bd);
        bd.calibrating   = false;
        bd.cal_countdown = 0;
        broker_mag_write(&bd);
    }
}
