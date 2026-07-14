/**
 * @file drv2605.h
 * @brief DRV2605 haptic motor driver — Core 0 only.
 *
 * Hardware: DRV2605 clone confirmed at 0x5A, WHO_AM_I = 0xE4 (unfused part).
 *           WHO_AM_I mismatch is advisory — the driver works regardless.
 * Motor type: LRA (Linear Resonant Actuator / Apple Taptic Engine).
 * Interface: I2C_NUM_1 (bus 2, GPIO4 SDA / GPIO5 SCL) — caller must hold
 *            g_i2c2_mutex. The Mk I move from bus 1 to bus 2 isolates haptic
 *            sequencer traffic from the sensor crowd on bus 1.
 *
 * Auto-calibration (DRV2605 internal BEMF) is confirmed non-functional with
 * the Apple Taptic Engine — DIAG bit always fails. IMU-assisted sweep
 * calibration via run_sweep() is the primary cal path.
 *
 * Architecture: Blueprint 1 §8, Blueprint 5 §2, Blueprint 13
 */

#ifndef DRV2605_H
#define DRV2605_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "driver/i2c.h"

// ── I2C address ───────────────────────────────────────────────────────────────
#define DRV2605_I2C_ADDR        0x5A

// ── Register map ──────────────────────────────────────────────────────────────
#define DRV2605_REG_STATUS      0x00
#define DRV2605_REG_MODE        0x01
#define DRV2605_REG_RTP         0x02
#define DRV2605_REG_LIBRARY     0x03
#define DRV2605_REG_WAVESEQ1    0x04
#define DRV2605_REG_WAVESEQ2    0x05
#define DRV2605_REG_GO          0x0C
#define DRV2605_REG_OVERDRIVE   0x0D
#define DRV2605_REG_SUSTAIN_POS 0x0E
#define DRV2605_REG_SUSTAIN_NEG 0x0F
#define DRV2605_REG_BRAKE       0x10
#define DRV2605_REG_RATED_VOLT  0x16
#define DRV2605_REG_OD_CLAMP    0x17
#define DRV2605_REG_CAL_COMP    0x18
#define DRV2605_REG_CAL_BEMF    0x19
#define DRV2605_REG_FEEDBACK    0x1A
#define DRV2605_REG_CONTROL1    0x1B
#define DRV2605_REG_CONTROL2    0x1C
#define DRV2605_REG_CONTROL3    0x1D
#define DRV2605_REG_CONTROL4    0x1E
#define DRV2605_REG_LRA_PERIOD  0x22

// ── Mode values ───────────────────────────────────────────────────────────────
#define DRV2605_MODE_INTTRIG    0x00
#define DRV2605_MODE_RTP        0x05
#define DRV2605_MODE_AUTOCAL    0x07
#define DRV2605_MODE_STANDBY    0x40

// ── Library ───────────────────────────────────────────────────────────────────
#define DRV2605_LIB_LRA         0x06

// ── Named effect IDs ──────────────────────────────────────────────────────────
// UI_EFFECT_COUNT must match the roller entries in haptic_tile.c exactly.
#define HAPTIC_EFFECT_CLICK           1
#define HAPTIC_EFFECT_CLICK_SOFT      3
#define HAPTIC_EFFECT_DOUBLE_CLICK    10
#define HAPTIC_EFFECT_TRIPLE_CLICK    11
#define HAPTIC_EFFECT_SOFT_BUMP       4
#define HAPTIC_EFFECT_STRONG_BUZZ     14
#define HAPTIC_EFFECT_SOFT_BUZZ       20
#define HAPTIC_EFFECT_ALERT_750MS     47
#define HAPTIC_EFFECT_TRANSITION_CLICK 49
#define HAPTIC_EFFECT_PULSING_SHARP   69
#define HAPTIC_EFFECT_LONG_BUZZ       56
#define HAPTIC_EFFECT_SHORT_DOUBLE    88
#define HAPTIC_EFFECT_SHARP_TICK      91

#define HAPTIC_UI_EFFECT_DEFAULT      HAPTIC_EFFECT_CLICK

// ── Sweep calibration constants ───────────────────────────────────────────────
// OD_LRA_PERIOD: period_s = reg × 98.46 µs  →  f_Hz = 1/(reg × 98.46e-6)
// reg=50 → ~203 Hz,  reg=64 → ~157 Hz (Apple Taptic target),  reg=90 → ~113 Hz
// Step size 1 = ~41 steps total; step size could be reduced for finer sweep.
#define DRV2605_SWEEP_REG_MIN   50
#define DRV2605_SWEEP_REG_MAX   90
#define DRV2605_SWEEP_STEPS     ((DRV2605_SWEEP_REG_MAX) - (DRV2605_SWEEP_REG_MIN) + 1)
#define DRV2605_SWEEP_BURST_MS  120
#define DRV2605_SWEEP_PAUSE_MS  80

// Frequency from period register.
// NEVER pass result directly to lv_label_set_text_fmt("%f"). snprintf first.
#define DRV2605_REG_TO_HZ(reg)  (1.0f / ((reg) * 98.46e-6f))

// ── IMU sweep countdown ───────────────────────────────────────────────────────
#define DRV2605_SWEEP_COUNTDOWN_S  5   // seconds to place device flat before sweep

// ── Command queue type ────────────────────────────────────────────────────────

typedef enum {
    HAPTIC_CMD_PLAY         = 0,
    HAPTIC_CMD_PLAY_FORCED  = 4,   // alarm — skips enabled check
    HAPTIC_CMD_SWEEP_START  = 1,
    HAPTIC_CMD_SWEEP_SET    = 2,
    HAPTIC_CMD_RTP_AMP      = 3,
} haptic_cmd_type_t;

typedef struct {
    uint8_t cmd;
    uint8_t param;
} haptic_cmd_t;

// ── Broker data struct ────────────────────────────────────────────────────────
typedef struct {
    // Calibration state
    bool     calibrated;
    bool     calibrating;
    uint8_t  last_effect;
    float    resonant_freq_hz;

    // Sweep progress
    bool     sweep_active;
    uint8_t  sweep_step;
    float    sweep_current_hz;
    float    sweep_last_amp;      // IMU Z amplitude at current step (0 = no IMU)

    // Countdown (IMU sweep flat-surface wait)
    bool     sweep_countdown;     // true while countdown is ticking
    uint8_t  sweep_countdown_sec; // seconds remaining (5 → 0)

    // Mandatory bookkeeping (Blueprint 4 §3)
    uint32_t last_update_ms;
    bool     enabled;
} broker_haptic_data_t;

#define BROKER_HAPTIC_TIMEOUT_MS  5000

// ── Public API ────────────────────────────────────────────────────────────────

esp_err_t drv2605_init(i2c_port_t i2c_num);
esp_err_t drv2605_calibrate(i2c_port_t i2c_num);
esp_err_t drv2605_get_cal_freq(i2c_port_t i2c_num, float *out_hz);
esp_err_t drv2605_play_effect(i2c_port_t i2c_num, uint8_t effect);
esp_err_t drv2605_stop(i2c_port_t i2c_num);
esp_err_t drv2605_set_period(i2c_port_t i2c_num, uint8_t period_reg);
esp_err_t drv2605_sweep_step(i2c_port_t i2c_num, uint8_t period_reg);

// ── Identity ──────────────────────────────────────────────────────────────────
const char *haptic_get_chip_name(void);
const char *haptic_get_chip_desc(void);

#endif // DRV2605_H