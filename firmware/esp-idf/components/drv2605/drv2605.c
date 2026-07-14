/**
 * @file drv2605.c
 * @brief DRV2605 haptic motor driver implementation -- Core 0 only.
 *
 * Clone-specific init:
 *   The DRV2605 clone at 0x5A reads WHO_AM_I = 0xE0 and is "unfused" --
 *   all calibration coefficients are zeroed. We must:
 *     1. Exit standby (MODE bit6 clear)
 *     2. Set library to LRA (6)
 *     3. Configure FEEDBACK for LRA mode
 *     4. Configure CONTROL1/2/3/4 for open-loop LRA
 *     5. Set rated voltage and OD clamp
 *
 * Phase 10 additions:
 *   drv2605_set_period()  -- write OD_LRA_PERIOD directly (apply NVS-stored value).
 *   drv2605_sweep_step()  -- play a short RTP burst at a given period register
 *                            value for manual resonance sweep calibration.
 *                            Uses open-loop LRA RTP mode (MODE=0x05) so the
 *                            motor is driven at exactly the requested frequency
 *                            rather than a library waveform.
 *
 * I2C: caller must hold g_i2c_mutex for every public function.
 * Core 0 only. No LVGL. No FreeRTOS task created here.
 */

#include "drv2605.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "DRV2605";

// ---------------------------------------------------------------------------
// Identity
// ---------------------------------------------------------------------------

const char *haptic_get_chip_name(void) { return "DRV2605"; }
const char *haptic_get_chip_desc(void) { return "LRA haptic driver"; }

// ---------------------------------------------------------------------------
// Low-level I2C helpers (file-scope only)
// ---------------------------------------------------------------------------

static esp_err_t write_reg(i2c_port_t port, uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    return i2c_master_write_to_device(port, DRV2605_I2C_ADDR,
                                      buf, 2, pdMS_TO_TICKS(20));
}

static esp_err_t read_reg(i2c_port_t port, uint8_t reg, uint8_t *out)
{
    return i2c_master_write_read_device(port, DRV2605_I2C_ADDR,
                                        &reg, 1, out, 1,
                                        pdMS_TO_TICKS(20));
}

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------

esp_err_t drv2605_init(i2c_port_t port)
{
    esp_err_t ret;

    // Sketch-verbatim sequence from 7_demo_field_capture:
    //   MODE=AUTOCAL first (so subsequent config lands inside cal mode),
    //   config regs, GO=1, poll GO up to 1500 ms, read STATUS to check
    //   DIAG_RESULT, then MODE=INTTRIG + LIBRARY=LRA for playback.

    // 1. Enter auto-cal mode.
    ret = write_reg(port, DRV2605_REG_MODE, DRV2605_MODE_AUTOCAL);
    if (ret != ESP_OK) { ESP_LOGE(TAG, "MODE=AUTOCAL failed: %s", esp_err_to_name(ret)); return ret; }

    // 2. FEEDBACK 0xB6 (LRA closed loop, factory brake/loop/BEMF).
    ret = write_reg(port, DRV2605_REG_FEEDBACK, 0xB6);
    if (ret != ESP_OK) { ESP_LOGE(TAG, "FEEDBACK failed"); return ret; }

    // 3. RATED_VOLTAGE 0x49  (sketch DRV_RATED_VOLTAGE).
    ret = write_reg(port, DRV2605_REG_RATED_VOLT, 0x49);
    if (ret != ESP_OK) { ESP_LOGE(TAG, "RATED_VOLT failed"); return ret; }

    // 4. OD_CLAMP 0x60      (sketch DRV_OD_CLAMP_REG).
    ret = write_reg(port, DRV2605_REG_OD_CLAMP, 0x60);
    if (ret != ESP_OK) { ESP_LOGE(TAG, "OD_CLAMP failed"); return ret; }

    // 5. CTRL4 0x30         (AUTO_CAL_TIME = 1000 ms window).
    ret = write_reg(port, DRV2605_REG_CONTROL4, 0x30);
    if (ret != ESP_OK) { ESP_LOGE(TAG, "CTRL4 failed"); return ret; }

    // 6. CTRL1 0x9C         (drive time + startup boost bit).
    ret = write_reg(port, DRV2605_REG_CONTROL1, 0x9C);
    if (ret != ESP_OK) { ESP_LOGE(TAG, "CTRL1 failed"); return ret; }

    // 7. GO=1 to kick off auto-cal.
    ret = write_reg(port, DRV2605_REG_GO, 0x01);
    if (ret != ESP_OK) { ESP_LOGE(TAG, "GO failed"); return ret; }

    // 8. Poll GO up to 1500 ms (sketch window).
    uint8_t go = 1;
    int64_t t0 = esp_timer_get_time();
    while (go && (esp_timer_get_time() - t0) < 1500000LL) {
        vTaskDelay(pdMS_TO_TICKS(50));
        if (read_reg(port, DRV2605_REG_GO, &go) != ESP_OK) { go = 1; break; }
        go &= 0x01;
    }

    // 9. STATUS bit 3 = DIAG_RESULT (0 = cal PASS, 1 = FAIL).
    uint8_t st = 0xFF;
    read_reg(port, DRV2605_REG_STATUS, &st);
    bool cal_ok = (go == 0) && !(st & 0x08);
    uint32_t cal_ms = (uint32_t)((esp_timer_get_time() - t0) / 1000LL);
    ESP_LOGI(TAG, "DRV2605 auto-cal %s in %u ms (STATUS=0x%02X)",
             cal_ok ? "PASS" : "FAIL", (unsigned)cal_ms, st);

    // 10. Back to internal-trigger mode.
    ret = write_reg(port, DRV2605_REG_MODE, DRV2605_MODE_INTTRIG);
    if (ret != ESP_OK) { ESP_LOGE(TAG, "MODE=INTTRIG failed"); return ret; }

    // 11. Waveform library = LRA.
    ret = write_reg(port, DRV2605_REG_LIBRARY, DRV2605_LIB_LRA);
    if (ret != ESP_OK) { ESP_LOGE(TAG, "LIBRARY failed"); return ret; }

    ESP_LOGI(TAG, "DRV2605 init OK (0x5A, LRA closed-loop, ELV1411A, sketch-verbatim)");
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// Auto-calibration
// Note: Apple Taptic Engine typically fails DIAG -- use sweep cal instead.
// Kept here for diagnostics and future ERM/standard LRA support.
// ---------------------------------------------------------------------------

esp_err_t drv2605_calibrate(i2c_port_t port)
{
    esp_err_t ret;

    ESP_LOGI(TAG, "Auto-calibration starting (~1.2 s)...");

    ret = write_reg(port, DRV2605_REG_MODE, DRV2605_MODE_AUTOCAL);
    if (ret != ESP_OK) { ESP_LOGE(TAG, "Cal mode set failed"); return ret; }

    ret = write_reg(port, DRV2605_REG_GO, 0x01);
    if (ret != ESP_OK) { ESP_LOGE(TAG, "GO bit set failed"); return ret; }

    // Poll GO bit until clear -- timeout 2 s
    const int max_polls = 40;
    for (int i = 0; i < max_polls; i++) {
        vTaskDelay(pdMS_TO_TICKS(50));
        uint8_t go = 0;
        ret = read_reg(port, DRV2605_REG_GO, &go);
        if (ret != ESP_OK) { ESP_LOGW(TAG, "GO poll read error"); continue; }
        if ((go & 0x01) == 0) {
            ESP_LOGI(TAG, "Calibration complete after ~%d ms", (i + 1) * 50);
            break;
        }
        if (i == max_polls - 1) {
            ESP_LOGE(TAG, "Calibration timed out");
            write_reg(port, DRV2605_REG_MODE, 0x00);
            return ESP_FAIL;
        }
    }

    uint8_t status = 0;
    ret = read_reg(port, DRV2605_REG_STATUS, &status);
    if (ret != ESP_OK) { ESP_LOGE(TAG, "Status read failed"); return ret; }

    if (status & (1 << 3)) {
        ESP_LOGE(TAG, "Auto-cal DIAG failed (STATUS=0x%02X)", status);
        write_reg(port, DRV2605_REG_MODE, 0x00);
        return ESP_FAIL;
    }

    uint8_t comp = 0, bemf = 0, period = 0;
    read_reg(port, DRV2605_REG_CAL_COMP,  &comp);
    read_reg(port, DRV2605_REG_CAL_BEMF,  &bemf);
    read_reg(port, DRV2605_REG_LRA_PERIOD, &period);
    ESP_LOGI(TAG, "Cal OK: COMP=0x%02X BEMF=0x%02X PERIOD=0x%02X STATUS=0x%02X",
             comp, bemf, period, status);

    ret = write_reg(port, DRV2605_REG_MODE, DRV2605_MODE_INTTRIG);
    if (ret != ESP_OK) { ESP_LOGE(TAG, "Restore mode failed"); return ret; }

    return ESP_OK;
}

// ---------------------------------------------------------------------------
// Frequency readback
// ---------------------------------------------------------------------------

esp_err_t drv2605_get_cal_freq(i2c_port_t port, float *out_hz)
{
    uint8_t period = 0;
    esp_err_t ret = read_reg(port, DRV2605_REG_LRA_PERIOD, &period);
    if (ret != ESP_OK) return ret;

    if (period == 0) {
        *out_hz = 0.0f;
        return ESP_OK;
    }

    *out_hz = DRV2605_REG_TO_HZ(period);
    ESP_LOGI(TAG, "Resonant freq: %.1f Hz (period reg=0x%02X)", (double)*out_hz, period);
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// Set period register directly
//
// Writes OD_LRA_PERIOD. Call this at boot to apply a period loaded from NVS,
// or after sweep calibration to lock in the chosen frequency.
// ---------------------------------------------------------------------------

esp_err_t drv2605_set_period(i2c_port_t port, uint8_t period_reg)
{
    if (period_reg == 0) return ESP_ERR_INVALID_ARG;
    esp_err_t ret = write_reg(port, DRV2605_REG_LRA_PERIOD, period_reg);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "OD_LRA_PERIOD set to 0x%02X (%.1f Hz)",
                 period_reg, (double)DRV2605_REG_TO_HZ(period_reg));
    }
    return ret;
}

// ---------------------------------------------------------------------------
// Sweep calibration step
//
// Drives the LRA at the frequency corresponding to period_reg for
// DRV2605_SWEEP_BURST_MS milliseconds, then stops.
//
// Mechanism:
//   1. Write OD_LRA_PERIOD to set drive frequency.
//   2. Switch to RTP mode (MODE=0x05) -- Real-Time Playback bypasses the
//      waveform library sequencer and drives the motor continuously at the
//      amplitude written to REG_RTP (0x02). With CONTROL3 bit2=1 (open-loop
//      LRA already set in init), the output frequency equals 1/(period*98.46us).
//   3. Write RTP amplitude (0x7F = ~50% -- enough to feel without harsh shock).
//   4. Delay for burst duration.
//   5. Stop: write RTP=0, return to internal-trigger mode.
//
// This blocks for DRV2605_SWEEP_BURST_MS ms -- call from Core 0 task only.
// Caller holds g_i2c_mutex for the entire call.
//
// IMU stub: a future IMU-assisted version would read accelerometer amplitude
// here during the burst and return it as a quality metric, eliminating the
// need for manual "feel" selection.
// ---------------------------------------------------------------------------

esp_err_t drv2605_sweep_step(i2c_port_t port, uint8_t period_reg)
{
    esp_err_t ret;

    // 1. Set the drive frequency
    ret = write_reg(port, DRV2605_REG_LRA_PERIOD, period_reg);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Sweep: period write failed (reg=0x%02X)", period_reg);
        return ret;
    }

    // 2. Switch to RTP mode
    ret = write_reg(port, DRV2605_REG_MODE, DRV2605_MODE_RTP);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Sweep: RTP mode set failed");
        return ret;
    }

    // 3. Set RTP amplitude -- 0x7F (~50% full scale)
    ret = write_reg(port, DRV2605_REG_RTP, 0x7F);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Sweep: RTP amplitude write failed");
        write_reg(port, DRV2605_REG_MODE, DRV2605_MODE_INTTRIG);
        return ret;
    }

    ESP_LOGD(TAG, "Sweep step: period=0x%02X (%.1f Hz), burst %d ms",
             period_reg, (double)DRV2605_REG_TO_HZ(period_reg),
             DRV2605_SWEEP_BURST_MS);

    // 4. Drive for burst duration
    vTaskDelay(pdMS_TO_TICKS(DRV2605_SWEEP_BURST_MS));

    // 5. Stop: zero amplitude, return to internal-trigger mode
    write_reg(port, DRV2605_REG_RTP, 0x00);
    ret = write_reg(port, DRV2605_REG_MODE, DRV2605_MODE_INTTRIG);

    return ret;
}

// ---------------------------------------------------------------------------
// Play effect (waveform library)
// ---------------------------------------------------------------------------

esp_err_t drv2605_play_effect(i2c_port_t port, uint8_t effect)
{
    esp_err_t ret;

    ret = write_reg(port, DRV2605_REG_MODE, DRV2605_MODE_INTTRIG);
    if (ret != ESP_OK) return ret;

    ret = write_reg(port, DRV2605_REG_WAVESEQ1, effect);
    if (ret != ESP_OK) return ret;

    ret = write_reg(port, DRV2605_REG_WAVESEQ2, 0x00);
    if (ret != ESP_OK) return ret;

    ret = write_reg(port, DRV2605_REG_GO, 0x01);
    if (ret != ESP_OK) return ret;

    ESP_LOGD(TAG, "Playing effect %d", effect);
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// Stop
// ---------------------------------------------------------------------------

esp_err_t drv2605_stop(i2c_port_t port)
{
    return write_reg(port, DRV2605_REG_GO, 0x00);
}