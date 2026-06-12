/**
 * @file haptic.c
 * @brief Haptic module task -- Core 0. DRV2605 command queue consumer.
 *
 * CRASH FIX (Phase 11):
 *   s_haptic_queue created unconditionally at top of haptic_init().
 *
 * IMU-assisted sweep (Phase 11 / Phase 13):
 *   run_sweep() with s_imu_mode=true:
 *     1. 5-second countdown (broker sweep_countdown=true, sweep_countdown_sec ticks).
 *     2. Sweep all steps; after each burst reads broker_imu accel_z amplitude.
 *     3. Writes sweep_last_amp to broker each step — tile shows live graph.
 *     4. Auto-selects step with peak amplitude, latches to NVS.
 *   Manual mode (s_imu_mode=false): user presses "SET FREQ" to latch.
 *
 * UI effect (Phase 13):
 *   s_ui_effect_id: loaded from NVS at init, returned by haptic_get_ui_effect().
 *   All UI interaction callers use haptic_play(haptic_get_ui_effect()).
 *
 * NVS namespace "haptic_cfg":
 *   key "lra_period" (uint8_t) — calibrated LRA period register value
 *   key "ui_effect"  (uint8_t) — UI feedback effect ID
 *
 * Phase 4 bus move:
 *   Mk I puts DRV2605 on I2C bus 2 (GPIO4/5), separate from the bus-1 sensor
 *   crowd. All g_i2c_mutex acquisitions in this file are now g_i2c2_mutex,
 *   and the I2C port is I2C_NUM_1. The extern g_i2c2_mutex declaration is
 *   expected to land in boot_hw_init.{c,h}; until then the symbol is
 *   referenced via boot_hw_init.h (forward-extern; see DEFECT log).
 *
 * Core 0 only. No lv_* calls.
 */

#include "haptic.h"
#include "drv2605.h"
#include "data_broker.h"
#include "boot_hw_init.h"
#include "driver/i2c.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include <math.h>

static const char *TAG          = "HAPTIC";
static const char *NVS_NS       = "haptic_cfg";
static const char *NVS_KEY_PER  = "lra_period";
static const char *NVS_KEY_EFF  = "ui_effect";

// ---------------------------------------------------------------------------
// Module state
// ---------------------------------------------------------------------------

static QueueHandle_t s_haptic_queue  = NULL;
static volatile bool s_cal_requested = false;
static volatile bool s_imu_mode      = true;   // IMU mode on by default

static bool     s_calibrated    = false;
static float    s_cal_freq_hz   = 0.0f;
static uint8_t  s_last_effect   = 0;
static uint8_t  s_saved_period  = 0;
static uint8_t  s_ui_effect_id  = HAPTIC_UI_EFFECT_DEFAULT;

static bool          s_sweep_active  = false;
static uint8_t       s_sweep_step    = 0;
static volatile bool s_sweep_set_req = false;

// ---------------------------------------------------------------------------
// NVS helpers
// ---------------------------------------------------------------------------

static void nvs_load_all(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGI(TAG, "No NVS config (first boot)");
        return;
    }

    // LRA period
    uint8_t period = 0;
    if (nvs_get_u8(h, NVS_KEY_PER, &period) == ESP_OK
        && period >= DRV2605_SWEEP_REG_MIN
        && period <= DRV2605_SWEEP_REG_MAX) {
        s_saved_period = period;
        s_cal_freq_hz  = DRV2605_REG_TO_HZ(period);
        s_calibrated   = true;
        ESP_LOGI(TAG, "Loaded LRA period: 0x%02X (%.0f Hz)", period, (double)s_cal_freq_hz);
    }

    // UI effect
    uint8_t eff = 0;
    if (nvs_get_u8(h, NVS_KEY_EFF, &eff) == ESP_OK && eff >= 1 && eff <= 123) {
        s_ui_effect_id = eff;
        ESP_LOGI(TAG, "Loaded UI effect: %d", eff);
    }

    nvs_close(h);
}

static void nvs_save_period(uint8_t period)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    if (nvs_set_u8(h, NVS_KEY_PER, period) == ESP_OK) nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "Saved LRA period: 0x%02X", period);
}

static void nvs_save_ui_effect(uint8_t eff)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    if (nvs_set_u8(h, NVS_KEY_EFF, eff) == ESP_OK) nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "Saved UI effect: %d", eff);
}

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------

esp_err_t haptic_init(void)
{
    s_haptic_queue = xQueueCreate(HAPTIC_QUEUE_DEPTH, sizeof(haptic_cmd_t));
    if (!s_haptic_queue) {
        ESP_LOGE(TAG, "Queue create FAILED");
        return ESP_ERR_NO_MEM;
    }

    if (xSemaphoreTake(g_i2c2_mutex, pdMS_TO_TICKS(200)) != pdTRUE) {
        ESP_LOGW(TAG, "I2C mutex timeout — hardware init skipped");
        return ESP_OK;
    }

    esp_err_t ret = drv2605_init(I2C_NUM_1);
    if (ret == ESP_OK) {
        nvs_load_all();
        if (s_saved_period > 0) {
            drv2605_set_period(I2C_NUM_1, s_saved_period);
        }
        ESP_LOGI(TAG, "Haptic init OK, UI effect=%d", s_ui_effect_id);
    } else {
        ESP_LOGE(TAG, "DRV2605 init failed — offline mode");
        nvs_load_all();   // still load UI effect even if HW absent
        ret = ESP_OK;
    }

    xSemaphoreGive(g_i2c2_mutex);
    return ret;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void haptic_play(uint8_t effect_id)
{
    if (!s_haptic_queue) return;
    haptic_cmd_t cmd = { .cmd = HAPTIC_CMD_PLAY, .param = effect_id };
    xQueueSend(s_haptic_queue, &cmd, 0);
}

void haptic_play_forced(uint8_t effect_id)
{
    if (!s_haptic_queue) return;
    haptic_cmd_t cmd = { .cmd = HAPTIC_CMD_PLAY_FORCED, .param = effect_id };
    xQueueSend(s_haptic_queue, &cmd, 0);
}

void haptic_set_rtp_amp(uint8_t amp_7bit)
{
    if (!s_haptic_queue) return;
    haptic_cmd_t cmd = { .cmd = HAPTIC_CMD_RTP_AMP, .param = amp_7bit };
    xQueueSend(s_haptic_queue, &cmd, 0);
}

uint8_t haptic_get_ui_effect(void)
{
    return s_ui_effect_id;
}

void haptic_set_ui_effect(uint8_t effect_id)
{
    if (effect_id < 1 || effect_id > 123) return;
    s_ui_effect_id = effect_id;
    nvs_save_ui_effect(effect_id);
    // Play the new effect immediately so user can feel the preview
    haptic_play(effect_id);
}

void haptic_sweep_start(void)
{
    if (!s_haptic_queue) return;
    haptic_cmd_t cmd = { .cmd = HAPTIC_CMD_SWEEP_START, .param = 0 };
    xQueueSend(s_haptic_queue, &cmd, 0);
}

void haptic_sweep_set(void)
{
    s_sweep_set_req = true;
}

void haptic_sweep_set_imu_mode(bool enable)
{
    s_imu_mode = enable;
    ESP_LOGI(TAG, "IMU sweep mode: %s", enable ? "ON" : "OFF");
}

void haptic_request_calibration(void)
{
    s_cal_requested = true;
}

// ---------------------------------------------------------------------------
// Broker write
// ---------------------------------------------------------------------------

static void write_broker(bool calibrating, bool countdown, uint8_t countdown_sec,
                         float last_amp)
{
    broker_haptic_data_t bd = {0};
    broker_haptic_read(&bd);
    bd.calibrated         = s_calibrated;
    bd.calibrating        = calibrating;
    bd.last_effect        = s_last_effect;
    bd.resonant_freq_hz   = s_cal_freq_hz;
    bd.sweep_active       = s_sweep_active;
    bd.sweep_step         = s_sweep_step;
    bd.sweep_current_hz   = s_sweep_active
                            ? DRV2605_REG_TO_HZ(DRV2605_SWEEP_REG_MIN + s_sweep_step)
                            : s_cal_freq_hz;
    bd.sweep_last_amp     = last_amp;
    bd.sweep_countdown    = countdown;
    bd.sweep_countdown_sec = countdown_sec;
    broker_haptic_write(&bd);
}

// ---------------------------------------------------------------------------
// IMU amplitude helper
// ---------------------------------------------------------------------------

static float imu_z_amplitude(void)
{
    broker_imu_data_t imu = {0};
    broker_imu_read(&imu);
    return fabsf(imu.accel_z);
}

// ---------------------------------------------------------------------------
// Sweep execution
// ---------------------------------------------------------------------------

static void run_sweep(void)
{
    char buf[16];

    // -- IMU countdown: place flat for DRV2605_SWEEP_COUNTDOWN_S seconds ----
    if (s_imu_mode && broker_imu_hw_alive()) {
        ESP_LOGI(TAG, "Sweep: %ds countdown — place device flat",
                 DRV2605_SWEEP_COUNTDOWN_S);
        for (int t = DRV2605_SWEEP_COUNTDOWN_S; t > 0; t--) {
            write_broker(false, true, (uint8_t)t, 0.0f);
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
        write_broker(false, false, 0, 0.0f);
    }

    ESP_LOGI(TAG, "Sweep: %d steps, reg %d..%d", DRV2605_SWEEP_STEPS,
             DRV2605_SWEEP_REG_MIN, DRV2605_SWEEP_REG_MAX);

    s_sweep_active  = true;
    s_sweep_set_req = false;
    s_sweep_step    = 0;
    write_broker(false, false, 0, 0.0f);

    float   best_amp  = -1.0f;
    uint8_t best_step = 0;
    float   last_amp  = 0.0f;

    for (uint8_t step = 0; step < DRV2605_SWEEP_STEPS; step++) {
        // Manual mode: check for early SET between steps
        if (!s_imu_mode && s_sweep_set_req) goto latch;

        s_sweep_step = step;
        uint8_t reg  = (uint8_t)(DRV2605_SWEEP_REG_MIN + step);
        write_broker(false, false, 0, last_amp);

        if (xSemaphoreTake(g_i2c2_mutex, pdMS_TO_TICKS(500)) == pdTRUE) {
            drv2605_sweep_step(I2C_NUM_1, reg);
            xSemaphoreGive(g_i2c2_mutex);
        } else {
            ESP_LOGW(TAG, "Sweep step %d: mutex timeout", step);
        }

        // IMU path: sample Z amplitude after brief settle
        if (s_imu_mode && broker_imu_hw_alive()) {
            vTaskDelay(pdMS_TO_TICKS(20));
            float amp = imu_z_amplitude();
            last_amp  = amp;
            if (amp > best_amp) { best_amp = amp; best_step = step; }
            snprintf(buf, sizeof(buf), "%.3f", (double)amp);
            ESP_LOGI(TAG, "IMU step %d (reg=%d, %.0f Hz): Z=%s m/s²",
                     step, reg,
                     (double)DRV2605_REG_TO_HZ(reg),
                     buf);
            // Write amplitude immediately so tile shows live bar
            write_broker(false, false, 0, amp);
        }

        vTaskDelay(pdMS_TO_TICKS(DRV2605_SWEEP_PAUSE_MS));

        if (!s_imu_mode && s_sweep_set_req) goto latch;
    }

    if (s_imu_mode && best_amp > 0.0f) {
        s_sweep_step = best_step;
        snprintf(buf, sizeof(buf), "%.0f",
                 (double)DRV2605_REG_TO_HZ(DRV2605_SWEEP_REG_MIN + best_step));
        ESP_LOGI(TAG, "IMU auto-select: step %d (%s Hz), peak amp=%.3f",
                 best_step, buf, (double)best_amp);
        goto latch;
    }

    // Sweep finished with no SET / no IMU peak
    ESP_LOGI(TAG, "Sweep complete — no frequency latched");
    s_sweep_active = false;
    write_broker(false, false, 0, 0.0f);
    return;

latch:
    {
        uint8_t chosen = (uint8_t)(DRV2605_SWEEP_REG_MIN + s_sweep_step);
        float   hz     = DRV2605_REG_TO_HZ(chosen);
        snprintf(buf, sizeof(buf), "%.0f", (double)hz);
        ESP_LOGI(TAG, "Sweep SET: reg=0x%02X (%s Hz)", chosen, buf);

        if (xSemaphoreTake(g_i2c2_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            drv2605_set_period(I2C_NUM_1, chosen);
            xSemaphoreGive(g_i2c2_mutex);
        }

        nvs_save_period(chosen);
        s_saved_period  = chosen;
        s_cal_freq_hz   = hz;
        s_calibrated    = true;
        s_sweep_active  = false;
        s_sweep_set_req = false;
        write_broker(false, false, 0, 0.0f);
        haptic_play(HAPTIC_EFFECT_DOUBLE_CLICK);
    }
}

// ---------------------------------------------------------------------------
// Task
// ---------------------------------------------------------------------------
void task_haptic_fn(void *arg)
{
    ESP_LOGI(TAG, "Haptic task started (Core %d)", xPortGetCoreID());

    if (!s_haptic_queue) {
        ESP_LOGE(TAG, "Queue NULL — haptic_init() not called. Exiting.");
        vTaskDelete(NULL);
        return;
    }

    write_broker(false, false, 0, 0.0f);

    while (1) {
        if (s_cal_requested) {
            s_cal_requested = false;
            s_calibrated    = false;
            s_cal_freq_hz   = 0.0f;
            write_broker(true, false, 0, 0.0f);

            if (xSemaphoreTake(g_i2c2_mutex, pdMS_TO_TICKS(300)) == pdTRUE) {
                esp_err_t ret = drv2605_calibrate(I2C_NUM_1);
                if (ret == ESP_OK) {
                    drv2605_get_cal_freq(I2C_NUM_1, &s_cal_freq_hz);
                    s_calibrated = true;
                }
                xSemaphoreGive(g_i2c2_mutex);
            }

            write_broker(false, false, 0, 0.0f);
            continue;
        }

        haptic_cmd_t cmd = {0};
        if (xQueueReceive(s_haptic_queue, &cmd, pdMS_TO_TICKS(500)) == pdTRUE) {
            switch ((haptic_cmd_type_t)cmd.cmd) {

                case HAPTIC_CMD_SWEEP_START:
                    run_sweep();
                    break;

                case HAPTIC_CMD_SWEEP_SET:
                    s_sweep_set_req = true;
                    break;

                case HAPTIC_CMD_RTP_AMP:
                    if (!broker_haptic_hw_alive()) break;
                    if (xSemaphoreTake(g_i2c2_mutex, pdMS_TO_TICKS(30)) == pdTRUE) {
                        uint8_t data[2] = { DRV2605_REG_RTP, cmd.param };
                        i2c_master_write_to_device(I2C_NUM_1, DRV2605_I2C_ADDR,
                                                   data, 2, pdMS_TO_TICKS(20));
                        xSemaphoreGive(g_i2c2_mutex);
                    }
                    break;
                
                case HAPTIC_CMD_PLAY_FORCED:
                    if (!broker_haptic_hw_alive()) break;
                    if (xSemaphoreTake(g_i2c2_mutex, pdMS_TO_TICKS(30)) == pdTRUE) {
                        esp_err_t ret = drv2605_play_effect(I2C_NUM_1, cmd.param);
                        xSemaphoreGive(g_i2c2_mutex);
                        if (ret == ESP_OK) {
                            s_last_effect = cmd.param;
                            ESP_LOGD(TAG, "Forced play: effect %u", (unsigned)cmd.param);
                        }
                    }
                    break;

                case HAPTIC_CMD_PLAY:
                default:
                    if (!broker_haptic_hw_alive()) break;
                    if (!broker_haptic_get_enabled()) break;
                    if (xSemaphoreTake(g_i2c2_mutex, pdMS_TO_TICKS(30)) == pdTRUE) {
                        esp_err_t ret = drv2605_play_effect(I2C_NUM_1, cmd.param);
                        xSemaphoreGive(g_i2c2_mutex);
                        if (ret == ESP_OK) s_last_effect = cmd.param;
                    }
                    break;
            }
        }

        write_broker(false, false, 0, 0.0f);
    }
}