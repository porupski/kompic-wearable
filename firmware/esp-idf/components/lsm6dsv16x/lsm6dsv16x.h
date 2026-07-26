/**
 * @file lsm6dsv16x.h
 * @brief ST LSM6DSV16X 6-axis IMU -- I2C bus 1, INT1 motion wake.
 *
 * Hardware:
 *   Chip       : ST LSM6DSV16X (accel + gyro + embedded functions)
 *   I2C bus    : 1  (GPIO1 SDA, GPIO2 SCL, 400 kHz)
 *   I2C addr   : 0x6B (fixed)
 *   INT1 pin   : GPIO8 (RTC-wake-capable; wake-on-motion / raise-to-wake)
 *   INT2 pin   : not routed on Mk I -- ignore
 *   WHO_AM_I   : 0x70 at REG_WHO_AM_I (0x0F)
 *   Power      : VDD 3V3 (analog), V_IO 1V8 (digital -- per v7.2 line 233)
 *
 * Replaces the old QMI8658 driver verbatim at the broker-payload + tile-API
 * level. The broker_imu_data_t shape, the complementary-filter logic, and
 * the haptic.c accel_z consumer all carry forward unchanged. Only the chip
 * layer (WHO_AM_I, register map, scaling) is new.
 *
 * Architecture: Blueprint 1 §1 (Core 0 owns I2C), Blueprint 4 §3 (read-before-write).
 *
 * Datasheet: ST DocID DS13176 / AN5824. Register addresses tagged [DSV]
 * (datasheet-verify) below are family-conventional for the LSM6DSV / DSV16X
 * line; bench bring-up confirms.
 */

#ifndef LSM6DSV16X_H
#define LSM6DSV16X_H


// Driver version: MAJOR.MINOR.PATCH -- bump PATCH on any change here,
// MINOR on feature adds, MAJOR on release quality (beta / RC / GA).
#define LSM6DSV16X_DRIVER_VERSION  "0.3.0"
#include "esp_err.h"
#include "driver/i2c.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdint.h>
#include <stdbool.h>

// -- Identity (used by imu_tile.c) --------------------------------------------
const char *lsm6dsv16x_get_chip_name(void);  // returns "LSM6DSV16X"
const char *lsm6dsv16x_get_chip_desc(void);  // returns "6-axis IMU + sensor fusion"

// -- Wire-level constants -----------------------------------------------------
#define LSM6DSV16X_I2C_ADDR       0x6B
#define LSM6DSV16X_WHO_AM_I_VAL   0x70
#define LSM6DSV16X_INT1_GPIO      GPIO_NUM_8

#define LSM6DSV16X_POLL_MS        20    // 50 Hz -- watch use case; gyro ODR set higher

// -- Register map (per ST DS13176; [DSV] -- bench confirms) -------------------
#define REG_WHO_AM_I              0x0F
#define REG_PIN_CTRL              0x02
#define REG_FIFO_CTRL1            0x07
#define REG_INT1_CTRL             0x0D
#define REG_INT2_CTRL             0x0E
#define REG_CTRL1                 0x10  // accel ODR + FS
#define REG_CTRL2                 0x11  // gyro  ODR + FS
#define REG_CTRL3                 0x12  // BDU, IF_INC, SW_RESET, BOOT
#define REG_CTRL4                 0x13
#define REG_CTRL5                 0x14
#define REG_CTRL6                 0x15
#define REG_CTRL7                 0x16
#define REG_CTRL8                 0x17
#define REG_CTRL9                 0x18
#define REG_CTRL10                0x19
#define REG_OUT_TEMP_L            0x20
#define REG_OUT_TEMP_H            0x21
#define REG_OUTX_L_G              0x22  // gyro X low byte; burst-read 12 bytes for XYZ_G + XYZ_XL
#define REG_FUNC_CFG_ACCESS       0x01
#define REG_WAKE_UP_THS           0x5B  // wake threshold [DSV]
#define REG_WAKE_UP_DUR           0x5C
#define REG_TAP_CFG0              0x56
#define REG_TAP_CFG1              0x57
#define REG_TAP_CFG2              0x58
#define REG_MD1_CFG               0x5E  // INT1 routing for embedded functions

// -- Additional main-bank registers used by pedometer / tap / activity -------
// DS13510 rev 4 §9.44-9.65. Section refs inline at point of use in _emb.c.
#define REG_WAKE_UP_SRC           0x45  // §9.44 -- SLEEP_STATE, SLEEP_CHANGE_IA
#define REG_TAP_SRC               0x46  // §9.45 -- TAP_IA, SINGLE/DOUBLE_TAP, axis bits
#define REG_EMB_FUNC_STATUS_MP    0x49  // §9.49 -- IS_STEP_DET, IS_TILT, IS_SIGMOT
#define REG_MLC_STATUS_MP         0x4B  // §9.51 -- IS_MLC1..4
#define REG_FUNCTIONS_ENABLE      0x50  // §9.53 -- INACT_EN[3:2], TIMESTAMP_EN
#define REG_INACTIVITY_DUR        0x54  // §9.55 -- activity/inactivity duration
#define REG_INACTIVITY_THS        0x55  // §9.56 -- activity threshold (WK_THS[5:0])
#define REG_TAP_THS_6D            0x59  // §9.60 -- TAP_THS_Z[4:0], SIXD_THS, D4D_EN
#define REG_TAP_DUR               0x5A  // §9.61 -- DUR[7:4], QUIET[3:2], SHOCK[1:0]

// -- Embedded-functions bank register addresses (visible when
// FUNC_CFG_ACCESS.EMB_FUNC_REG_ACCESS = 1). DS13510 rev 4 §13. --------------
#define REG_EMB_EMB_FUNC_EN_A     0x04  // §13.2 -- PEDO_EN bit 3, TILT_EN bit 4, SIGN_MOTION_EN bit 5
#define REG_EMB_EMB_FUNC_EN_B     0x05  // §13.3 -- MLC_EN, FSM_EN
#define REG_EMB_EMB_FUNC_INT1     0x0A  // §13.7 -- INT1_STEP_DETECTOR bit 3
#define REG_EMB_STEP_COUNTER_L    0x62  // pedometer count (LE, 16-bit)
#define REG_EMB_STEP_COUNTER_H    0x63
#define REG_EMB_EMB_FUNC_SRC      0x64  // pedometer reset via PEDO_RST_STEP bit 7
#define REG_EMB_EMB_FUNC_INIT_A   0x66  // SFLP_GAME_INIT etc.
#define REG_EMB_MLC1_SRC          0x70  // MLC classifier 1 output byte

// -- Control-register bit positions (subset; [DSV]) ---------------------------
#define CTRL3_SW_RESET            (1 << 0)
#define CTRL3_IF_INC              (1 << 2)
#define CTRL3_BDU                 (1 << 6)
#define CTRL3_BOOT                (1 << 7)

// -- LSM6DSV16X-correct register layouts (Stage 11 Item C audit) --------------
// Datasheet: ST DS13510 rev 4.
//
// The pre-Stage-11 macros above (CTRL1_ODR_120HZ etc.) were inherited from
// the LSM6DSO layout, where FS_XL lived in CTRL1[3:2] and ODR_XL in [7:4].
// On the LSM6DSV16X those fields are laid out differently:
//   * FS_XL   is on CTRL8 (0x17) bits [1:0]         -- not CTRL1
//   * FS_G    is on CTRL6 (0x15) bits [3:0]         -- not CTRL2
//   * OP_MODE_XL is on CTRL1 bits [6:4]
//   * ODR_XL  is on CTRL1 bits [3:0]
//   * OP_MODE_G  is on CTRL2 bits [6:4]
//   * ODR_G   is on CTRL2 bits [3:0]
//
// Writing the DSO layout to a DSV16X put OP_MODE_XL=Low-Power-3 (110) and
// ODR_XL=7.5 Hz (0010). Bench-confirmed on 2026-07-24: MLC accel rows
// repeated every ~140 ms (7 Hz), az reported ~19.5 m/s^2 at rest (~2x g).

// CTRL1 (0x10): [7]=0 | [6:4]=OP_MODE_XL | [3:0]=ODR_XL
#define CTRL1_OP_MODE_HP          (0x00 << 4)  // High-Performance
#define CTRL1_OP_MODE_HA_ODR      (0x01 << 4)  // High-Accuracy ODR
#define CTRL1_OP_MODE_ODR_TRIG    (0x03 << 4)  // ODR-triggered
#define CTRL1_OP_MODE_LP1         (0x04 << 4)
#define CTRL1_OP_MODE_LP2         (0x05 << 4)
#define CTRL1_OP_MODE_LP3         (0x06 << 4)  // <- was silently active pre-Item-C
#define CTRL1_OP_MODE_NORMAL      (0x07 << 4)
#define CTRL1_ODR_XL_POWER_DOWN   (0x00)
#define CTRL1_ODR_XL_7_5HZ        (0x02)       // LP-mode-only actually; used pre-Item-C
#define CTRL1_ODR_XL_15HZ         (0x03)
#define CTRL1_ODR_XL_30HZ         (0x04)
#define CTRL1_ODR_XL_60HZ         (0x05)
#define CTRL1_ODR_XL_120HZ        (0x06)
#define CTRL1_ODR_XL_240HZ        (0x07)
#define CTRL1_ODR_XL_480HZ        (0x08)
#define CTRL1_ODR_XL_960HZ        (0x09)

// CTRL2 (0x11): [7]=0 | [6:4]=OP_MODE_G | [3:0]=ODR_G
#define CTRL2_OP_MODE_HP          (0x00 << 4)
#define CTRL2_OP_MODE_HA_ODR      (0x01 << 4)
#define CTRL2_OP_MODE_ODR_TRIG    (0x03 << 4)
#define CTRL2_OP_MODE_SLEEP       (0x04 << 4)
#define CTRL2_OP_MODE_LP          (0x05 << 4)
#define CTRL2_ODR_G_POWER_DOWN    (0x00)
#define CTRL2_ODR_G_7_5HZ         (0x02)
#define CTRL2_ODR_G_15HZ          (0x03)
#define CTRL2_ODR_G_30HZ          (0x04)
#define CTRL2_ODR_G_60HZ          (0x05)
#define CTRL2_ODR_G_120HZ         (0x06)
#define CTRL2_ODR_G_240HZ         (0x07)
#define CTRL2_ODR_G_480HZ         (0x08)
#define CTRL2_ODR_G_960HZ         (0x09)

// CTRL6 (0x15): [3:0] FS_G
#define CTRL6_FS_G_125DPS         (0x00)
#define CTRL6_FS_G_250DPS         (0x01)
#define CTRL6_FS_G_500DPS         (0x02)
#define CTRL6_FS_G_1000DPS        (0x03)
#define CTRL6_FS_G_2000DPS        (0x04)
#define CTRL6_FS_G_4000DPS        (0x0C)

// CTRL8 (0x17): [1:0] FS_XL
#define CTRL8_FS_XL_2G            (0x00)
#define CTRL8_FS_XL_4G            (0x01)
#define CTRL8_FS_XL_8G            (0x02)
#define CTRL8_FS_XL_16G           (0x03)

// INT1_CTRL: bit 0 DRDY_XL, bit 1 DRDY_G; INT1_CTRL bit 7 enables embedded INT routing
#define INT1_DRDY_XL              (1 << 0)
#define INT1_DRDY_G               (1 << 1)

// MD1_CFG: bit 5 = INT1_WU (wake-up routed to INT1)
#define MD1_INT1_WU               (1 << 5)

// TAP_CFG0: bit 0 LIR (latched interrupt -- recommended for wake-up use)
#define TAP_CFG0_LIR              (1 << 0)
// TAP_CFG2: bit 7 = INTERRUPTS_ENABLE
#define TAP_CFG2_INT_EN           (1 << 7)

// -- Bits for embedded functions + tap subsystem ------------------------------
// FUNC_CFG_ACCESS (0x01) bit 7 = enter embedded bank
#define FUNC_CFG_EMB_ACCESS       (1 << 7)

// EMB_FUNC_EN_A (embedded 0x04): bit 3 = PEDO_EN (§13.2)
#define EMB_FUNC_EN_A_PEDO_EN     (1 << 3)

// EMB_FUNC_SRC (embedded 0x64): bit 7 = PEDO_RST_STEP (write 1 to clear counter)
#define EMB_FUNC_SRC_PEDO_RST     (1 << 7)

// FUNCTIONS_ENABLE (0x50), INACT_EN[3:2]:
//   00 = no auto-switch, 01 = LP accel only, 10 = LP accel + gyro sleep,
//   11 = LP accel + gyro power-down.
#define FUNC_ENABLE_INACT_EN_OFF        (0x00 << 2)
#define FUNC_ENABLE_INACT_EN_LP_ONLY    (0x01 << 2)
#define FUNC_ENABLE_INACT_LP_GYRO_SLEEP (0x02 << 2)
#define FUNC_ENABLE_INACT_LP_GYRO_PD    (0x03 << 2)

// INACTIVITY_DUR (0x54):
//   [7]   SLEEP_STATUS_ON_INT
//   [6:4] WU_INACT_THS_W (mg per LSB of INACTIVITY_THS)
//   [3:2] XL_INACT_ODR   (accel ODR during sleep)
//   [1:0] INACT_DUR      (wake-back debounce, N events)
#define INACT_DUR_WEIGHT_62MG5    (0x03 << 4)  // 62.5 mg/LSB
#define INACT_DUR_ODR_15HZ        (0x01 << 2)
#define INACT_DUR_WAKE_2EVENTS    (0x01 << 0)

// TAP_CFG0 layout (§9.57): 0 | LOW_PASS_ON_6D | HW_FUNC_MASK | SLOPE_FDS |
//                          TAP_X_EN | TAP_Y_EN | TAP_Z_EN | LIR
#define TAP_CFG0_TAP_X_EN         (1 << 3)
#define TAP_CFG0_TAP_Y_EN         (1 << 2)
#define TAP_CFG0_TAP_Z_EN         (1 << 1)

// WAKE_UP_THS (0x5B) bit 7 = enable single AND double tap events
#define WAKE_UP_THS_SINGLE_DOUBLE_TAP  (1 << 7)

// TAP_SRC (0x46) bit layout (§9.45)
#define TAP_SRC_TAP_IA            (1 << 6)
#define TAP_SRC_SINGLE_TAP        (1 << 5)
#define TAP_SRC_DOUBLE_TAP        (1 << 4)
#define TAP_SRC_TAP_SIGN          (1 << 3)   // 0 = positive, 1 = negative
#define TAP_SRC_X_TAP             (1 << 2)
#define TAP_SRC_Y_TAP             (1 << 1)
#define TAP_SRC_Z_TAP             (1 << 0)

// WAKE_UP_SRC (0x45)
#define WAKE_UP_SRC_SLEEP_CHANGE_IA (1 << 5)
#define WAKE_UP_SRC_SLEEP_STATE     (1 << 3)

// -- Scaling (ST datasheet typical; ±4g accel + ±2000 dps gyro) ---------------
// Accel: at ±4g full-scale, sensitivity is 0.122 mg/LSB = 8192 LSB/g.
#define ACCEL_LSB_PER_G           8192.0f
// Gyro: at ±2000 dps full-scale, sensitivity is 70 mdps/LSB ~= 14.29 LSB/dps.
#define GYRO_LSB_PER_DPS          14.286f
// Temperature: 256 LSB/°C with 25 °C offset.
#define TEMP_LSB_PER_C            256.0f
#define TEMP_OFFSET_C             25.0f

#define GRAVITY_MS2               9.80665f

// -- Broker data struct (Blueprint 4 §3) --------------------------------------
// Field names and order preserved EXACTLY from the old qmi8658.h so that
// imu_tile.c, haptic.c, and any other consumer build without changes.
// haptic.c specifically reads broker_imu_data_t.accel_z during sweep
// calibration (it peaks at the LRA resonant frequency).
typedef struct {
    // Accelerometer (m/s²) -- gravity-corrected, ±4g range
    float accel_x;
    float accel_y;
    float accel_z;

    // Gyroscope (°/s) -- ±2000 dps range
    float gyro_x;
    float gyro_y;
    float gyro_z;

    // Complementary-filtered orientation (computed in task, not in tile)
    float roll_deg;
    float pitch_deg;

    // On-chip temperature (°C)
    float temperature;

    // Mandatory bookkeeping (Blueprint 4 §3)
    uint32_t last_update_ms;
    bool     enabled;
} broker_imu_data_t;

#define BROKER_IMU_TIMEOUT_MS  500U

// -- Pedometer broker struct --------------------------------------------------
// The LSM's embedded pedometer runs on its own 30 Hz pipeline independent of
// the host accel ODR (as long as accel is >= 30 Hz). Counter is a 16-bit
// on-chip register widened to 32-bit host-side with rollover tracking.
// task_imu_fn polls once per second when broker_steps is enabled.
typedef struct {
    uint32_t step_count;         // widened 32-bit; ship-mode resets to 0
    uint32_t last_update_ms;
    bool     enabled;
} broker_steps_data_t;

#define BROKER_STEPS_TIMEOUT_MS  2000U

// -- Lifecycle ----------------------------------------------------------------

/**
 * @brief Verify WHO_AM_I (0x70), soft-reset, configure accel + gyro + INT1.
 *        Caller must NOT hold g_i2c_mutex (takes it internally).
 *        Called from boot_hw_init.c after I2C scan confirms 0x6B alive.
 * @return ESP_OK on success.
 */
esp_err_t lsm6dsv16x_init(i2c_port_t i2c_num);

/**
 * @brief FreeRTOS task. Pinned to Core 0 via boot_tasks.c.
 *        Polls at LSM6DSV16X_POLL_MS interval, applies complementary filter,
 *        writes to broker. Read-before-write so UI-owned `enabled` is preserved.
 *        Stack: 4096 bytes.
 */
void task_imu_fn(void *arg);

/**
 * @brief Burst-read TEMP + GYRO + ACCEL (12 bytes total, plus 2 temp bytes
 *        from 0x20). Scales raw values into out's float fields. Caller holds
 *        g_i2c_mutex. Does NOT touch out->roll_deg/pitch_deg (filter math
 *        lives in the task).
 */
esp_err_t lsm6dsv16x_read(i2c_port_t i2c_num, broker_imu_data_t *out);

/**
 * @brief Configure GPIO8 as a falling-edge interrupt and install an ISR that
 *        notifies the given FreeRTOS task. Used for wake-on-motion. Idempotent.
 *        Pass NULL to uninstall. NOT thread-safe -- call from boot_hw_init.c
 *        before any task is created.
 */
esp_err_t lsm6dsv16x_install_int1_isr(TaskHandle_t notify_task);

// ---------------------------------------------------------------------------
// Advanced-features API (lsm6dsv16x_emb.c). Each function takes and releases
// g_i2c_mutex internally -- caller must NOT hold it. Safe to call from any
// Core 0 task context. All bank-switch protocol is contained within these
// helpers; embedded-bank writes never leak past the function boundary.
// ---------------------------------------------------------------------------

// Pedometer -- embedded 30 Hz pipeline. Requires accel ODR >= 30 Hz to count.
esp_err_t lsm6dsv16x_pedometer_enable(bool en);
esp_err_t lsm6dsv16x_pedometer_reset(void);                    // clear chip counter
esp_err_t lsm6dsv16x_pedometer_read(uint32_t *steps_out);      // widened 32-bit

// Activity/inactivity auto-sleep. When ON, the chip drops accel to LP1 at the
// configured sleep-ODR (default 15 Hz) once inactive; wakes on motion. Note:
// this is a no-op if the host is forcing HP mode at high ODR for MOTION/BCG.
esp_err_t lsm6dsv16x_activity_sleep_enable(bool en);
bool      lsm6dsv16x_is_sleeping(void);                        // reads WAKE_UP_SRC.SLEEP_STATE

// Tap-Z (both single and double) as a UI input. Poll returns the latched
// TAP_SRC byte or 0 if no event. The chip auto-clears on read when LIR=0.
esp_err_t lsm6dsv16x_tap_z_enable(bool en);
uint8_t   lsm6dsv16x_tap_poll_src(void);                       // 0 = no event

// Monotonic tap counters, updated by task_imu_fn on every observed event.
// UI consumers dedupe by remembering the last count they handled and only
// acting on a strict increase. This is the primary path field_capture uses
// to act on double-tap-Z (the "go back" gesture). The parallel ui_event_q
// send is best-effort for LVGL overlay use.
uint32_t  lsm6dsv16x_tap_z_single_count(void);
uint32_t  lsm6dsv16x_tap_z_double_count(void);

// MLC (Machine Learning Core) output register. Returns 0 until a .ucf model
// is loaded. Wired up here so the pipeline is testable end-to-end before a
// classifier is available.
uint8_t   lsm6dsv16x_mlc1_read(void);

// Load an ST Unico/MEMS-Studio ".ucf" classifier configuration. The .ucf
// text format is a sequence of lines:
//     Ac ADDR VAL       (two hex bytes, space-separated)
//     -- <comment>
// This function skips comments/blanks and applies every "Ac" write in order.
// The .ucf itself contains any bank-switch writes needed (typically wraps
// its content in FUNC_CFG_ACCESS = 0x80 / 0x00), so this loader is a
// straight write-through -- do NOT wrap it in emb_bank_enter/exit.
// Returns ESP_OK on success or the first failing i2c error.
esp_err_t lsm6dsv16x_mlc_load_ucf(const char *ucf_text, size_t len);

// Bank-switch helpers -- exposed for the MLC .ucf loader (lsm6dsv16x_mlc.c).
// Caller must NOT hold g_i2c_mutex. Every enter must be paired with an exit.
// Do NOT sprinkle calls to these across the codebase; keep advanced-feature
// register access confined to lsm6dsv16x_emb.c / _mlc.c.
esp_err_t lsm6dsv16x_emb_bank_enter(void);
esp_err_t lsm6dsv16x_emb_bank_exit(void);

#endif // LSM6DSV16X_H
