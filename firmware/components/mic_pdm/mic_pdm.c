/**
 * @file mic_pdm.c
 * @brief PDM RX channel on I2S0, 16 kHz / 16-bit mono.
 *
 * Why I2S0 and not I2S1?
 *   ESP32-S3 has two I2S controllers. I2S1 has the GDMA capabilities the
 *   display path eventually wants for parallel-RGB use, and PDM-RX is
 *   only supported on I2S0 anyway. The choice writes itself.
 *
 * Why 16 kHz / 16-bit?
 *   - 16 kHz is the bog-standard speech-recognition sample rate. Most
 *     PDM MEMS mics on the market are spec'd for ~16-48 kHz; 16 kHz
 *     hits the sweet spot of "wideband speech" without burning extra
 *     CPU on resampling later.
 *   - 16-bit gives ~96 dB dynamic range -- more than the typical MEMS
 *     mic's analog SNR of 60-70 dB, so the bottleneck is the chip.
 *
 * Why mono?
 *   v7.2 has one PDM mic. JP9 = GND selects the left channel; the right
 *   slot is unused. Some PDM driver libraries default to stereo and let
 *   you discard the right channel, which doubles the DMA traffic for
 *   nothing. We configure SLOT_MODE_MONO + SLOT_LEFT to match the wiring.
 *
 * DMA topology:
 *   6 frames * 20 ms = 120 ms total buffer depth in PSRAM. The IDF v5
 *   PDM RX channel handles ring management; we only `i2s_channel_read()`
 *   one frame at a time. At a 16-bit sample 20 ms = 640 bytes per frame.
 *
 * Concurrency:
 *   The IDF channel handle is single-consumer by design; we expose a
 *   single reader (`mic_pdm_read`) and trust the caller not to call it
 *   from two tasks concurrently. Init/start/stop/deinit are guarded by
 *   a private mutex because they touch the channel handle.
 */

#include "mic_pdm.h"

#include <string.h>
#include "esp_err.h"
#include "esp_log.h"
#include "driver/i2s_pdm.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "MIC_PDM";

const char *mic_pdm_get_chip_name(void) { return "PDM Mic";                       }
const char *mic_pdm_get_chip_desc(void) { return "I2S0 PDM RX 16kHz/16b, GPIO47/48"; }

// ─────────────────────────────────────────────────────────────────────────────
// State
// ─────────────────────────────────────────────────────────────────────────────

static SemaphoreHandle_t s_lock        = NULL;
static i2s_chan_handle_t s_rx_chan     = NULL;
static bool              s_initialised = false;
static bool              s_running     = false;

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────

static inline esp_err_t take(uint32_t timeout_ms)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
        if (!s_lock) return ESP_ERR_NO_MEM;
    }
    return (xSemaphoreTake(s_lock, pdMS_TO_TICKS(timeout_ms)) == pdTRUE)
        ? ESP_OK : ESP_ERR_TIMEOUT;
}
static inline void give(void) { if (s_lock) xSemaphoreGive(s_lock); }

// ─────────────────────────────────────────────────────────────────────────────
// Lifecycle
// ─────────────────────────────────────────────────────────────────────────────

esp_err_t mic_pdm_init(void)
{
    esp_err_t r = take(1000);
    if (r != ESP_OK) return r;

    if (s_initialised) { give(); return ESP_OK; }

    ESP_LOGI(TAG, "Init I2S0 PDM RX on CLK=%d DIN=%d @ %d Hz, %d-bit, mono",
             MIC_PDM_GPIO_CLK, MIC_PDM_GPIO_DIN,
             MIC_PDM_SAMPLE_RATE_HZ, MIC_PDM_BITS_PER_SAMPLE);

    // 6 frames of 320 samples each -- 120 ms total buffered.
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num  = MIC_PDM_DMA_FRAME_COUNT;
    chan_cfg.dma_frame_num = MIC_PDM_FRAME_SAMPLES;
    chan_cfg.auto_clear    = true;

    r = i2s_new_channel(&chan_cfg, NULL, &s_rx_chan);
    if (r != ESP_OK) {
        ESP_LOGE(TAG, "i2s_new_channel: %s", esp_err_to_name(r));
        give();
        return r;
    }

    i2s_pdm_rx_config_t pdm_cfg = {
        .clk_cfg  = I2S_PDM_RX_CLK_DEFAULT_CONFIG(MIC_PDM_SAMPLE_RATE_HZ),
        .slot_cfg = I2S_PDM_RX_SLOT_DEFAULT_CONFIG(
                        I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .clk = MIC_PDM_GPIO_CLK,
            .din = MIC_PDM_GPIO_DIN,
            .invert_flags = {
                .clk_inv = false,
            },
        },
    };
    // JP9 = GND selects left channel; match the hardware.
    pdm_cfg.slot_cfg.slot_mask = I2S_PDM_SLOT_LEFT;

    r = i2s_channel_init_pdm_rx_mode(s_rx_chan, &pdm_cfg);
    if (r != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_init_pdm_rx_mode: %s", esp_err_to_name(r));
        i2s_del_channel(s_rx_chan);
        s_rx_chan = NULL;
        give();
        return r;
    }

    s_initialised = true;
    ESP_LOGI(TAG, "PDM RX init OK (channel allocated, not yet started)");
    give();
    return ESP_OK;
}

esp_err_t mic_pdm_deinit(void)
{
    esp_err_t r = take(1000);
    if (r != ESP_OK) return r;

    if (s_running) {
        (void)i2s_channel_disable(s_rx_chan);
        s_running = false;
    }
    if (s_rx_chan) {
        (void)i2s_del_channel(s_rx_chan);
        s_rx_chan = NULL;
    }
    s_initialised = false;
    ESP_LOGI(TAG, "PDM RX deinit");
    give();
    return ESP_OK;
}

esp_err_t mic_pdm_start(void)
{
    esp_err_t r = take(1000);
    if (r != ESP_OK) return r;
    if (!s_initialised) { give(); return ESP_ERR_INVALID_STATE; }
    if (s_running)      { give(); return ESP_OK; }

    r = i2s_channel_enable(s_rx_chan);
    if (r == ESP_OK) s_running = true;
    else ESP_LOGE(TAG, "i2s_channel_enable: %s", esp_err_to_name(r));
    give();
    return r;
}

esp_err_t mic_pdm_stop(void)
{
    esp_err_t r = take(1000);
    if (r != ESP_OK) return r;
    if (!s_running) { give(); return ESP_OK; }
    r = i2s_channel_disable(s_rx_chan);
    if (r == ESP_OK) s_running = false;
    give();
    return r;
}

bool mic_pdm_is_running(void) { return s_running; }

// ─────────────────────────────────────────────────────────────────────────────
// Capture API
// ─────────────────────────────────────────────────────────────────────────────

esp_err_t mic_pdm_read(void *dst, size_t bytes_to_read,
                       size_t *out_bytes_read, uint32_t timeout_ms)
{
    if (!dst || !bytes_to_read || !out_bytes_read) return ESP_ERR_INVALID_ARG;
    if (!s_initialised || !s_running)              return ESP_ERR_INVALID_STATE;
    return i2s_channel_read(s_rx_chan, dst, bytes_to_read,
                            out_bytes_read, pdMS_TO_TICKS(timeout_ms));
}

esp_err_t mic_pdm_capture(int16_t *dst, size_t n_frames, uint32_t timeout_ms)
{
    if (!dst || !n_frames) return ESP_ERR_INVALID_ARG;
    if (!s_initialised) {
        esp_err_t r = mic_pdm_init();
        if (r != ESP_OK) return r;
    }
    bool was_running = s_running;
    if (!was_running) {
        esp_err_t r = mic_pdm_start();
        if (r != ESP_OK) return r;
    }

    esp_err_t result = ESP_OK;
    for (size_t f = 0; f < n_frames; f++) {
        size_t bytes_read = 0;
        esp_err_t r = mic_pdm_read(dst + f * MIC_PDM_FRAME_SAMPLES,
                                    MIC_PDM_FRAME_BYTES, &bytes_read, timeout_ms);
        if (r != ESP_OK || bytes_read != MIC_PDM_FRAME_BYTES) {
            ESP_LOGW(TAG, "frame %u short read: %s, got %u/%u",
                     (unsigned)f, esp_err_to_name(r),
                     (unsigned)bytes_read, (unsigned)MIC_PDM_FRAME_BYTES);
            result = (r != ESP_OK) ? r : ESP_ERR_INVALID_RESPONSE;
            break;
        }
    }

    if (!was_running) (void)mic_pdm_stop();
    return result;
}
