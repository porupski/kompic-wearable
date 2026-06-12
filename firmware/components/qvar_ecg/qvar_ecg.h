/**
 * @file qvar_ecg.h
 * @brief Qvar (charge-variation) ECG capture via the LSM6DSV16X embedded block.
 *
 * Hardware (v7.2 §I2C BUS ASSIGNMENT, §CASE):
 *   The LSM6DSV16X (I2C bus 1, addr 0x6B) carries an embedded analog
 *   front-end called Qvar -- a capacitive / charge-variation channel
 *   intended for skin-contact biopotential measurement (ECG class).
 *
 *   Mk I wiring (v7.2 §CASE -- ECG electrodes):
 *     Qvar1 input (skin)   <- pogo pin through a printed sleeve in the
 *                             case bottom, touches a daughter-board pad.
 *     Qvar2 input (crown)  <- pogo pin riding a groove on the crown shaft
 *                             (2nd ECG contact).
 *
 *   No interrupt is wired. Sampling is firmware-pulled.
 *
 * Driver model:
 *   - Shares I2C bus 1 with the rest of the chip (g_i2c_mutex).
 *   - Reads OUT_AH_QVAR_{L,H} registers ([DSV] addresses) at a fixed
 *     ODR; LSM6DSV16X supports Qvar ODRs of 240 Hz / 480 Hz / 960 Hz.
 *     We use 240 Hz (cardiology floor; nyquist for 100 Hz signals).
 *   - On-demand capture: caller requests "1 s of ECG"; we read 240
 *     samples into an int16 ring buffer in PSRAM.
 *   - Impedance check: high impedance value at the Qvar input usually
 *     indicates poor / no electrode contact. The driver exposes a
 *     "do we have contact?" boolean based on the AH_QVAR_C_ZIN
 *     impedance-selection register's saturation behaviour ([DSV]).
 *
 * Critically: this module DOES NOT init the LSM6DSV16X chip. It assumes
 * the IMU driver has already brought up the chip. We only touch the
 * Qvar-specific control registers (CTRL7 bit AH_QVAR_EN, the embedded
 * function page registers) and OUT_AH_QVAR.
 *
 * Architecture: Blueprint 1 §3 (driver pattern), Blueprint 5 §5 (ECG).
 *
 * Brief: docs/18_PHASE_5_BATCH_ADVANCED.md, Module 3.
 */

#ifndef QVAR_ECG_H
#define QVAR_ECG_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "driver/i2c.h"

// -- LSM6DSV16X bus (same as imu) ---------------------------------------------
#define QVAR_I2C_ADDR        0x6B
#define QVAR_I2C_PORT        I2C_NUM_0   // bus 1 -- matches lsm6dsv16x.c

// -- Qvar registers (LSM6DSV16X datasheet, [DSV]) -----------------------------
#define QVAR_REG_CTRL7              0x16  // bit 0 = AH_QVAR_EN, bits 4-5 = ZIN [DSV]
#define QVAR_REG_OUT_AH_QVAR_L      0x3A  // [DSV]
#define QVAR_REG_OUT_AH_QVAR_H      0x3B  // [DSV]
#define QVAR_REG_FUNC_CFG_ACCESS    0x01  // bank select for embedded function pages

#define QVAR_CTRL7_AH_QVAR_EN       (1 << 0)  // [DSV]
// ZIN selection: 2.4 GOhm / 730 MOhm / 300 MOhm / 235 MOhm ([DSV]).
// Lower impedance = lower input-referred noise but more current draw.
// Mk I pick: 235 MOhm (lowest) -- skin contact through pogo pins is
// low-impedance enough that we don't need GOhm-class.
#define QVAR_CTRL7_ZIN_235M         (0x03 << 4)  // [DSV]

// -- Capture parameters -------------------------------------------------------
#define QVAR_ODR_HZ                 240       // 240 Hz -- cardiology-grade [DSV]
#define QVAR_DEFAULT_CAPTURE_SEC    1
#define QVAR_DEFAULT_CAPTURE_N      (QVAR_ODR_HZ * QVAR_DEFAULT_CAPTURE_SEC)
#define QVAR_SAMPLE_PERIOD_US       (1000000 / QVAR_ODR_HZ)

// -- Contact-detection threshold ----------------------------------------------
// A floating Qvar input rails. Treat |sample| > QVAR_FLOAT_THRESHOLD over
// a contiguous window as "no contact".
#define QVAR_FLOAT_THRESHOLD        30000
#define QVAR_CONTACT_WINDOW_SAMPLES 48      // 200 ms at 240 Hz

// -- Identity -----------------------------------------------------------------
const char *qvar_ecg_get_chip_name(void);   // "QvarECG"
const char *qvar_ecg_get_chip_desc(void);   // "LSM6DSV16X AH_QVAR @ 240 Hz"

// -- Lifecycle ----------------------------------------------------------------

/**
 * @brief Enable the Qvar block on the LSM6DSV16X. Assumes the IMU driver
 *        has already brought up the chip. Idempotent.
 *
 * @param i2c_num  I2C port number for bus 1 (typically I2C_NUM_0).
 */
esp_err_t qvar_ecg_init(i2c_port_t i2c_num);

/**
 * @brief Disable the Qvar block (CTRL7.AH_QVAR_EN = 0). Other LSM6DSV16X
 *        functions remain intact.
 */
esp_err_t qvar_ecg_deinit(void);

// -- Streaming sample API -----------------------------------------------------

/**
 * @brief Read one 16-bit Qvar sample (raw, LSB-of-the-input units).
 *        Takes g_i2c_mutex briefly. ESP_OK on success.
 */
esp_err_t qvar_ecg_read_sample(int16_t *out);

// -- Capture API --------------------------------------------------------------

/**
 * @brief Capture `n_samples` Qvar samples at the 240 Hz nominal rate
 *        into `dst`. Blocks for ~n_samples/240 seconds. Returns the
 *        number of samples actually captured via `out_n`.
 *
 *        Timing uses esp_timer + vTaskDelayUntil so the sample cadence
 *        is regular even if the I2C path occasionally stretches.
 */
esp_err_t qvar_ecg_capture(int16_t *dst, size_t n_samples,
                           size_t *out_n);

/**
 * @brief Heuristic contact-detection: capture QVAR_CONTACT_WINDOW_SAMPLES
 *        samples; if a majority pegs above QVAR_FLOAT_THRESHOLD, return
 *        false (= no skin contact). Otherwise true.
 *        Takes ~200 ms.
 */
bool qvar_ecg_has_contact(void);

#endif // QVAR_ECG_H
