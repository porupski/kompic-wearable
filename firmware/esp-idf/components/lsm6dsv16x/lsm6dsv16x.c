/**
 * @file lsm6dsv16x.c
 * @brief ST LSM6DSV16X driver -- Core 0 only.
 *
 * Replaces qmi8658.c at the chip layer. The complementary-filter logic in
 * task_imu_fn is the verbatim carry-forward from the QMI8658 implementation,
 * because the math is chip-independent.
 *
 * Scaling:
 *   Accel: ±4g full-scale, 0.122 mg/LSB -> 8192 LSB/g.
 *   Gyro:  ±2000 dps full-scale, 70 mdps/LSB -> ~14.29 LSB/dps.
 *   Temp:  256 LSB/°C with 25 °C offset.
 *
 * Read-before-write pattern (Blueprint 4 §3):
 *   Task reads broker before writing so the UI-owned `enabled` field is
 *   never clobbered. Same pattern as veml6030.c and haptic.c.
 */

#include "lsm6dsv16x.h"
#include "data_broker.h"
#include "driver/i2c.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <math.h>
#include <string.h>

extern SemaphoreHandle_t g_i2c_mutex;

static const char *TAG = "LSM6DSV16X";

// ---------------------------------------------------------------------------
// Complementary filter state (Core 0 only)
// ---------------------------------------------------------------------------
#define CF_ALPHA   0.98f
#define CF_DT      (LSM6DSV16X_POLL_MS / 1000.0f)

static float s_roll_deg     = 0.0f;
static float s_pitch_deg    = 0.0f;
static bool  s_filter_seeded = false;

// ---------------------------------------------------------------------------
// INT1 ISR plumbing
// ---------------------------------------------------------------------------
static TaskHandle_t s_int1_task   = NULL;
static volatile bool s_isr_added  = false;

static void IRAM_ATTR lsm_int1_isr(void *arg)
{
    (void)arg;
    BaseType_t hpw = pdFALSE;
    if (s_int1_task) {
        vTaskNotifyGiveFromISR(s_int1_task, &hpw);
    }
    if (hpw) portYIELD_FROM_ISR();
}

// ---------------------------------------------------------------------------
// Identity
// ---------------------------------------------------------------------------
const char *lsm6dsv16x_get_chip_name(void) { return "LSM6DSV16X"; }
const char *lsm6dsv16x_get_chip_desc(void) { return "6-axis IMU + sensor fusion"; }

// ---------------------------------------------------------------------------
// I2C primitives (caller holds g_i2c_mutex)
// ---------------------------------------------------------------------------
static esp_err_t write_reg(i2c_port_t port, uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    return i2c_master_write_to_device(port, LSM6DSV16X_I2C_ADDR,
                                      buf, 2, pdMS_TO_TICKS(20));
}

static esp_err_t read_reg(i2c_port_t port, uint8_t reg, uint8_t *out)
{
    return i2c_master_write_read_device(port, LSM6DSV16X_I2C_ADDR,
                                        &reg, 1, out, 1, pdMS_TO_TICKS(20));
}

static esp_err_t read_regs(i2c_port_t port, uint8_t reg, uint8_t *buf, size_t len)
{
    return i2c_master_write_read_device(port, LSM6DSV16X_I2C_ADDR,
                                        &reg, 1, buf, len, pdMS_TO_TICKS(20));
}

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------
esp_err_t lsm6dsv16x_init(i2c_port_t i2c_num)
{
    if (xSemaphoreTake(g_i2c_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    uint8_t chip_id = 0;
    esp_err_t ret = read_reg(i2c_num, REG_WHO_AM_I, &chip_id);
    if (ret != ESP_OK || chip_id != LSM6DSV16X_WHO_AM_I_VAL) {
        xSemaphoreGive(g_i2c_mutex);
        ESP_LOGE(TAG, "WHO_AM_I failed: got 0x%02X, expected 0x%02X",
                 chip_id, LSM6DSV16X_WHO_AM_I_VAL);
        return ESP_FAIL;
    }

    // Soft reset
    (void)write_reg(i2c_num, REG_CTRL3, CTRL3_SW_RESET);
    xSemaphoreGive(g_i2c_mutex);
    vTaskDelay(pdMS_TO_TICKS(20));

    if (xSemaphoreTake(g_i2c_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    // BDU = 1 (block data update), IF_INC = 1 (auto-increment for burst reads)
    (void)write_reg(i2c_num, REG_CTRL3, CTRL3_BDU | CTRL3_IF_INC);

    // Accel: 120 Hz, ±4g
    (void)write_reg(i2c_num, REG_CTRL1, CTRL1_ODR_120HZ | CTRL1_FS_XL_4G);

    // Gyro: 240 Hz, ±2000 dps
    (void)write_reg(i2c_num, REG_CTRL2, CTRL2_ODR_240HZ | CTRL2_FS_G_2000DPS);

    // INT1: enable DRDY_XL routing (useful for ISR-driven mode later).
    // For wake-on-motion, MD1_CFG bit 5 must also be set (done in install_int1_isr).
    (void)write_reg(i2c_num, REG_INT1_CTRL, 0x00);  // disabled by default; tile can enable

    xSemaphoreGive(g_i2c_mutex);
    vTaskDelay(pdMS_TO_TICKS(20));

    ESP_LOGI(TAG, "%s init OK @ 0x%02X (WHO_AM_I=0x%02X)",
             lsm6dsv16x_get_chip_name(), LSM6DSV16X_I2C_ADDR, chip_id);
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// Read + scale
// ---------------------------------------------------------------------------
esp_err_t lsm6dsv16x_read(i2c_port_t i2c_num, broker_imu_data_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;

    // 14 bytes from REG_OUT_TEMP_L (0x20):
    //   [0-1]   TEMP
    //   [2-7]   GYRO  X/Y/Z (low/high)
    //   [8-13]  ACCEL X/Y/Z (low/high)
    uint8_t raw[14];
    esp_err_t ret = read_regs(i2c_num, REG_OUT_TEMP_L, raw, sizeof(raw));
    if (ret != ESP_OK) return ret;

    int16_t t_raw  = (int16_t)((raw[1]  << 8) | raw[0]);
    int16_t gx_raw = (int16_t)((raw[3]  << 8) | raw[2]);
    int16_t gy_raw = (int16_t)((raw[5]  << 8) | raw[4]);
    int16_t gz_raw = (int16_t)((raw[7]  << 8) | raw[6]);
    int16_t ax_raw = (int16_t)((raw[9]  << 8) | raw[8]);
    int16_t ay_raw = (int16_t)((raw[11] << 8) | raw[10]);
    int16_t az_raw = (int16_t)((raw[13] << 8) | raw[12]);

    out->temperature = ((float)t_raw / TEMP_LSB_PER_C) + TEMP_OFFSET_C;
    out->accel_x     = ((float)ax_raw / ACCEL_LSB_PER_G) * GRAVITY_MS2;
    out->accel_y     = ((float)ay_raw / ACCEL_LSB_PER_G) * GRAVITY_MS2;
    out->accel_z     = ((float)az_raw / ACCEL_LSB_PER_G) * GRAVITY_MS2;
    out->gyro_x      = (float)gx_raw / GYRO_LSB_PER_DPS;
    out->gyro_y      = (float)gy_raw / GYRO_LSB_PER_DPS;
    out->gyro_z      = (float)gz_raw / GYRO_LSB_PER_DPS;
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// INT1 (wake-on-motion) -- ISR install/uninstall
// ---------------------------------------------------------------------------
esp_err_t lsm6dsv16x_install_int1_isr(TaskHandle_t notify_task)
{
    if (notify_task == NULL) {
        if (s_isr_added) {
            gpio_isr_handler_remove(LSM6DSV16X_INT1_GPIO);
            s_isr_added = false;
        }
        s_int1_task = NULL;
        return ESP_OK;
    }

    gpio_config_t io = {
        .pin_bit_mask = 1ULL << LSM6DSV16X_INT1_GPIO,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type    = GPIO_INTR_POSEDGE,   // [DSV] LSM6DSV16X INT idles LOW
    };
    esp_err_t ret = gpio_config(&io);
    if (ret != ESP_OK) return ret;

    esp_err_t svc = gpio_install_isr_service(ESP_INTR_FLAG_LEVEL1);
    if (svc != ESP_OK && svc != ESP_ERR_INVALID_STATE) return svc;

    s_int1_task = notify_task;
    ret = gpio_isr_handler_add(LSM6DSV16X_INT1_GPIO, lsm_int1_isr, NULL);
    if (ret != ESP_OK) {
        s_int1_task = NULL;
        return ret;
    }
    s_isr_added = true;
    ESP_LOGI(TAG, "INT1 ISR installed on GPIO%d (pos edge)", LSM6DSV16X_INT1_GPIO);
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// Task (Blueprint 4 §3 -- read-before-write)
// ---------------------------------------------------------------------------
void task_imu_fn(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "Task started on Core %d", xPortGetCoreID());

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(LSM6DSV16X_POLL_MS));

        if (!broker_imu_hw_alive())    continue;
        if (!broker_imu_get_enabled()) continue;

        broker_imu_data_t bd = {0};
        broker_imu_read(&bd);

        if (xSemaphoreTake(g_i2c_mutex, pdMS_TO_TICKS(50)) != pdTRUE) continue;
        esp_err_t ret = lsm6dsv16x_read(I2C_NUM_0, &bd);
        xSemaphoreGive(g_i2c_mutex);

        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Read failed: %s", esp_err_to_name(ret));
            continue;
        }

        // Complementary filter -- carried forward verbatim from qmi8658.c.
        float acc_roll  = atan2f(bd.accel_y, bd.accel_z) * (180.0f / (float)M_PI);
        float acc_pitch = atan2f(-bd.accel_x,
                                 sqrtf(bd.accel_y * bd.accel_y + bd.accel_z * bd.accel_z))
                          * (180.0f / (float)M_PI);

        if (!s_filter_seeded) {
            s_roll_deg      = acc_roll;
            s_pitch_deg     = acc_pitch;
            s_filter_seeded = true;
        } else {
            s_roll_deg  = CF_ALPHA * (s_roll_deg  + bd.gyro_x * CF_DT)
                          + (1.0f - CF_ALPHA) * acc_roll;
            s_pitch_deg = CF_ALPHA * (s_pitch_deg + bd.gyro_y * CF_DT)
                          + (1.0f - CF_ALPHA) * acc_pitch;
        }

        bd.roll_deg  = s_roll_deg;
        bd.pitch_deg = s_pitch_deg;

        broker_imu_write(&bd);
    }
}
