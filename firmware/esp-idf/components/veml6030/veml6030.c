/**
 * @file veml6030.c
 * @brief Vishay VEML6030 driver -- Core 0 only.
 *
 * Replaces bh1750.c at the chip layer. Key differences from BH1750:
 *   - 16-bit registers, LSB first on the wire.
 *   - Configurable gain + integration time (auto-ranging state machine here).
 *   - Lux requires conversion factor lookup (resolution table from datasheet).
 *
 * Auto-range strategy:
 *   - Start at gain=1/4x, IT=100 ms (resolution 0.2304 lx/count, max ~15.1 klux).
 *   - If raw >= SAT_HIGH (0xFFF0):   drop one notch (-> gain=1/8x).
 *   - If raw <= SAT_LOW  (100):      raise one notch (-> gain=1x or 2x).
 *   - Skip one sample after each range change (chip needs the next full
 *     integration period to settle).
 *
 * EMA filter:
 *   lux_filt = alpha * lux_filt + (1-alpha) * lux_raw, alpha = 0.85
 *   Pre-seeded on first valid read to avoid step-on artifacts.
 *
 * Read-before-write pattern (Blueprint 4 §3):
 *   Task reads broker before writing so the UI-owned `enabled` field is
 *   preserved.
 */

#include "veml6030.h"
#include "data_broker.h"
#include "boot_hw_init.h"   // g_i2c_mutex
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <math.h>
#include <string.h>

static const char *TAG = "VEML6030";

// =============================================================================
// Resolution table (Vishay AN84367 Table 5) -- lx/count
//   rows = gain (0=1/8x, 1=1/4x, 2=1x, 3=2x)
//   cols = IT   (0=25ms, 1=50ms, 2=100ms, 3=200ms, 4=400ms, 5=800ms)
// =============================================================================
const float veml6030_resolution_lx_per_count[4][6] = {
    // 1/8x        gain
    { 1.8432f, 0.9216f, 0.4608f, 0.2304f, 0.1152f, 0.0576f },
    // 1/4x        gain
    { 0.9216f, 0.4608f, 0.2304f, 0.1152f, 0.0576f, 0.0288f },
    // 1x          gain
    { 0.2304f, 0.1152f, 0.0576f, 0.0288f, 0.0144f, 0.0072f },
    // 2x          gain
    { 0.1152f, 0.0576f, 0.0288f, 0.0144f, 0.0072f, 0.0036f },
};

// =============================================================================
// Auto-range state
// =============================================================================
static veml6030_gain_t s_gain  = VEML6030_GAIN_1_4;
static veml6030_it_t   s_it    = VEML6030_IT_100MS;
static bool            s_skip_next = false;  // post-range-change settling

#define SAT_HIGH   0xFFF0
#define SAT_LOW    100

// EMA filter state (Core 0 only)
static float s_lux_ema    = 0.0f;
static bool  s_ema_seeded = false;
#define EMA_ALPHA  0.85f

// =============================================================================
// Identity
// =============================================================================
const char *veml6030_get_chip_name(void) { return "VEML6030"; }
const char *veml6030_get_chip_desc(void) { return "Ambient light (lux)"; }

// =============================================================================
// I2C helpers -- VEML6030 uses 16-bit registers, LSB first.
// Caller holds g_i2c_mutex.
// =============================================================================
static esp_err_t write_reg16(i2c_port_t port, uint8_t reg, uint16_t val)
{
    uint8_t buf[3] = { reg, (uint8_t)(val & 0xFF), (uint8_t)(val >> 8) };
    return i2c_master_write_to_device(port, VEML6030_ADDR,
                                      buf, 3, pdMS_TO_TICKS(20));
}

static esp_err_t read_reg16(i2c_port_t port, uint8_t reg, uint16_t *out)
{
    uint8_t buf[2] = { 0, 0 };
    esp_err_t ret = i2c_master_write_read_device(port, VEML6030_ADDR,
                                                 &reg, 1, buf, 2,
                                                 pdMS_TO_TICKS(20));
    if (ret == ESP_OK && out) {
        *out = (uint16_t)buf[0] | ((uint16_t)buf[1] << 8);
    }
    return ret;
}

// =============================================================================
// Range / config helpers
// =============================================================================
static uint16_t encode_gain(veml6030_gain_t g)
{
    switch (g) {
        case VEML6030_GAIN_1:   return CONF_GAIN_1;
        case VEML6030_GAIN_2:   return CONF_GAIN_2;
        case VEML6030_GAIN_1_8: return CONF_GAIN_1_8;
        case VEML6030_GAIN_1_4: default: return CONF_GAIN_1_4;
    }
}

static uint16_t encode_it(veml6030_it_t it)
{
    switch (it) {
        case VEML6030_IT_25MS:  return CONF_IT_25MS;
        case VEML6030_IT_50MS:  return CONF_IT_50MS;
        case VEML6030_IT_200MS: return CONF_IT_200MS;
        case VEML6030_IT_400MS: return CONF_IT_400MS;
        case VEML6030_IT_800MS: return CONF_IT_800MS;
        case VEML6030_IT_100MS: default: return CONF_IT_100MS;
    }
}

float veml6030_current_resolution(void)
{
    return veml6030_resolution_lx_per_count[s_gain][s_it];
}

void veml6030_get_range(veml6030_gain_t *gain_out, veml6030_it_t *it_out)
{
    if (gain_out) *gain_out = s_gain;
    if (it_out)   *it_out   = s_it;
}

esp_err_t veml6030_set_range(i2c_port_t i2c_num, veml6030_gain_t gain, veml6030_it_t it)
{
    uint16_t conf = encode_gain(gain) | encode_it(it);  // ALS_SD = 0 (powered on)
    esp_err_t ret = write_reg16(i2c_num, VEML6030_REG_ALS_CONF, conf);
    if (ret == ESP_OK) {
        s_gain = gain;
        s_it   = it;
        s_skip_next = true;
        ESP_LOGI(TAG, "range set: gain=%d it=%d res=%.4f lx/cnt",
                 (int)gain, (int)it, (double)veml6030_current_resolution());
    }
    return ret;
}

// =============================================================================
// Lifecycle
// =============================================================================
esp_err_t veml6030_init(i2c_port_t i2c_num)
{
    if (xSemaphoreTake(g_i2c_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return ESP_ERR_TIMEOUT;

    // Power down first (ALS_SD = 1) -- Vishay recommends this before any
    // reconfig to avoid ambiguous mid-integration state.
    (void)write_reg16(i2c_num, VEML6030_REG_ALS_CONF, CONF_ALS_SD);
    xSemaphoreGive(g_i2c_mutex);
    vTaskDelay(pdMS_TO_TICKS(5));

    if (xSemaphoreTake(g_i2c_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return ESP_ERR_TIMEOUT;
    esp_err_t ret = veml6030_set_range(i2c_num, VEML6030_GAIN_1_4, VEML6030_IT_100MS);
    xSemaphoreGive(g_i2c_mutex);

    // Wait the configured integration period before the first useful read.
    vTaskDelay(pdMS_TO_TICKS(150));

    ESP_LOGI(TAG, "%s init OK @ 0x%02X (gain=1/4x, IT=100ms)",
             veml6030_get_chip_name(), VEML6030_ADDR);
    return ret;
}

void veml6030_deinit(void)
{
    if (xSemaphoreTake(g_i2c_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        (void)write_reg16(I2C_NUM_0, VEML6030_REG_ALS_CONF, CONF_ALS_SD);
        xSemaphoreGive(g_i2c_mutex);
    }
    ESP_LOGI(TAG, "Powered down");
}

esp_err_t veml6030_read_raw(i2c_port_t i2c_num, uint16_t *raw_out)
{
    return read_reg16(i2c_num, VEML6030_REG_ALS, raw_out);
}

// =============================================================================
// Lux -> brightness curve (perceptual log10 mapping)
// =============================================================================
uint8_t veml6030_lux_to_brightness(float lux)
{
    if (lux < 0.0f) lux = 0.0f;
    const float LX_MIN_PCT_5  = 1.0f;    // lux corresponding to 5% backlight
    const float LX_MAX_PCT_100 = 5000.0f; // lux at 100% backlight
    const float LOG_MIN = 0.0f;            // log10(1)
    const float LOG_MAX = 3.69897f;        // log10(5000)

    float l = log10f(lux + 1.0f);
    float t = (l - LOG_MIN) / (LOG_MAX - LOG_MIN);  // 0..1
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;

    int pct = (int)(5.0f + t * 95.0f);  // floor 5%, ceiling 100%
    if (pct < 5)   pct = 5;
    if (pct > 100) pct = 100;
    (void)LX_MIN_PCT_5;
    (void)LX_MAX_PCT_100;
    return (uint8_t)pct;
}

// =============================================================================
// Auto-range step
// =============================================================================
static void auto_range_step(i2c_port_t i2c_num, uint16_t raw)
{
    if (raw >= SAT_HIGH) {
        // Saturating -- step down sensitivity. Prefer gain change first, then IT.
        if (s_gain == VEML6030_GAIN_2) {
            (void)veml6030_set_range(i2c_num, VEML6030_GAIN_1,   s_it);
        } else if (s_gain == VEML6030_GAIN_1) {
            (void)veml6030_set_range(i2c_num, VEML6030_GAIN_1_4, s_it);
        } else if (s_gain == VEML6030_GAIN_1_4) {
            (void)veml6030_set_range(i2c_num, VEML6030_GAIN_1_8, s_it);
        }
        // If already at 1/8x and 25 ms, the world is brighter than we can read.
    } else if (raw <= SAT_LOW) {
        if (s_gain == VEML6030_GAIN_1_8) {
            (void)veml6030_set_range(i2c_num, VEML6030_GAIN_1_4, s_it);
        } else if (s_gain == VEML6030_GAIN_1_4) {
            (void)veml6030_set_range(i2c_num, VEML6030_GAIN_1,   s_it);
        } else if (s_gain == VEML6030_GAIN_1) {
            (void)veml6030_set_range(i2c_num, VEML6030_GAIN_2,   s_it);
        }
    }
}

// =============================================================================
// Task
// =============================================================================
void task_light_fn(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "Task started on Core %d", xPortGetCoreID());

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(VEML6030_POLL_MS));
        if (!broker_light_hw_alive())    continue;
        if (!broker_light_get_enabled()) continue;

        broker_light_data_t bd = {0};
        broker_light_read(&bd);  // read-before-write

        uint16_t raw = 0;
        if (xSemaphoreTake(g_i2c_mutex, pdMS_TO_TICKS(50)) != pdTRUE) continue;
        esp_err_t ret = veml6030_read_raw(I2C_NUM_0, &raw);
        if (ret == ESP_OK && !s_skip_next) {
            // Adjust range BEFORE reading the next sample (the change takes one
            // integration period; we mark skip_next so we don't act on stale data).
            auto_range_step(I2C_NUM_0, raw);
        }
        xSemaphoreGive(g_i2c_mutex);

        if (ret != ESP_OK) continue;
        if (s_skip_next) { s_skip_next = false; continue; }

        float lux_raw = (float)raw * veml6030_current_resolution();

        if (!s_ema_seeded) {
            s_lux_ema    = lux_raw;
            s_ema_seeded = true;
        } else {
            s_lux_ema = EMA_ALPHA * s_lux_ema + (1.0f - EMA_ALPHA) * lux_raw;
        }

        bd.lux             = s_lux_ema;
        bd.auto_brightness = veml6030_lux_to_brightness(s_lux_ema);
        broker_light_write(&bd);
    }
}
