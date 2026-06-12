/**
 * @file mic_pdm.h
 * @brief PDM MEMS microphone capture -- I2S0 PDM RX, GPIO47/48.
 *
 * Hardware (v7.2 §GPIO ASSIGNMENT):
 *   GPIO47 -- Mic_CLK   (3V3 domain on the WROOM-1U-N16R8)
 *   GPIO48 -- Mic_Dout  (3V3 domain)
 *
 * Both pins are "Clean (no RTC)" -- the mic is therefore unavailable
 * during deep sleep. That's fine for a watch: voice capture is an
 * awake-only feature.
 *
 * Mic L/R select: v7.2 §SOLDER JUMPERS JP9 = GND (left). The PDM
 * configuration must match the jumper; SDM_SLOT_LEFT is the default.
 *
 * Capture model:
 *   - IDF v5 i2s_pdm_rx new-driver API.
 *   - Sample rate: 16 kHz, 16-bit mono. 16 kHz is the speech-recognition
 *     standard; the 24-bit option that some PDM decimators offer is
 *     wasted at this rate.
 *   - DMA double-buffered. Buffer size 20 ms = 320 samples = 640 bytes;
 *     6 buffers in PSRAM = ~3.8 KiB total. That's 120 ms of latency
 *     headroom -- plenty.
 *   - The driver does no VAD, no Speex, no compression. Capture only;
 *     downstream processing lives in higher-level modules (Phase 2+).
 *
 * Architecture: Blueprint 1 §3 (driver pattern), Blueprint 5 §3 (audio).
 *
 * Brief: docs/18_PHASE_5_BATCH_ADVANCED.md, Module 2.
 */

#ifndef MIC_PDM_H
#define MIC_PDM_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "driver/gpio.h"

// -- Pinout (v7.2 §GPIO ASSIGNMENT) -------------------------------------------
#define MIC_PDM_GPIO_CLK     GPIO_NUM_47
#define MIC_PDM_GPIO_DIN     GPIO_NUM_48

// -- Capture parameters -------------------------------------------------------
#define MIC_PDM_SAMPLE_RATE_HZ   16000
#define MIC_PDM_BITS_PER_SAMPLE  16
#define MIC_PDM_FRAME_MS         20                                          // speech-standard 20 ms frame
#define MIC_PDM_FRAME_SAMPLES    ((MIC_PDM_SAMPLE_RATE_HZ * MIC_PDM_FRAME_MS) / 1000) // 320
#define MIC_PDM_FRAME_BYTES      (MIC_PDM_FRAME_SAMPLES * (MIC_PDM_BITS_PER_SAMPLE / 8))
#define MIC_PDM_DMA_FRAME_COUNT  6                                           // 120 ms total buffered

// -- Identity (test harness + tiles) ------------------------------------------
const char *mic_pdm_get_chip_name(void);  // "PDM Mic"
const char *mic_pdm_get_chip_desc(void);  // "I2S0 PDM RX 16kHz/16b, GPIO47/48"

// -- Lifecycle ----------------------------------------------------------------

/**
 * @brief Install the I2S0 PDM RX channel (DMA + buffers) but DO NOT
 *        start capture. Idempotent.
 */
esp_err_t mic_pdm_init(void);

/**
 * @brief Tear down the channel. Safe to call when not initialised.
 */
esp_err_t mic_pdm_deinit(void);

/**
 * @brief Enable the channel (DMA running, samples flowing). After this
 *        returns, `mic_pdm_read()` will produce live audio.
 */
esp_err_t mic_pdm_start(void);

/**
 * @brief Disable the channel; clears the in-flight DMA buffers.
 */
esp_err_t mic_pdm_stop(void);

/** @brief True if start() has been called and not yet stopped. */
bool mic_pdm_is_running(void);

// -- Capture API --------------------------------------------------------------

/**
 * @brief Read up to `bytes_to_read` of 16-bit signed PCM into `dst`.
 *        Blocks up to `timeout_ms` waiting for the DMA driver. Returns
 *        the number of bytes actually written via `out_bytes_read`.
 *        Typical caller cadence: one MIC_PDM_FRAME_BYTES read per loop.
 */
esp_err_t mic_pdm_read(void *dst, size_t bytes_to_read,
                       size_t *out_bytes_read, uint32_t timeout_ms);

/**
 * @brief One-shot capture: read `n_frames` of MIC_PDM_FRAME_BYTES each
 *        into the provided buffer. `dst` must be at least
 *        n_frames * MIC_PDM_FRAME_BYTES bytes.
 *        Convenience wrapper for "record N ms of audio" use cases.
 *        Starts the channel if it's not running, leaves it in the same
 *        state it found it.
 */
esp_err_t mic_pdm_capture(int16_t *dst, size_t n_frames, uint32_t timeout_ms);

#endif // MIC_PDM_H
