/**
 * @file field_capture.h
 * @brief Port of the 7_demo_field_capture Arduino sketch to ESP-IDF.
 *
 * Encoder-driven mode selector, single-click trigger, double-click ship mode,
 * WS2812 status indicator, DRV haptic feedback, PDM mic voice annotation,
 * SDMMC-backed CSV / WAV recording. No display path.
 *
 * MODES (encoder cycles one detent = one step):
 *   MIC        30 s mono WAV capture at 16 kHz
 *   ENV        BME688 + VEML6030 CSV
 *   MOTION     LSM6DSV16X + LIS3MDLTR CSV
 *   SKIN       MAX30101 PPG + TMP117 CSV
 *   FLASHLIGHT LED toggle; encoder = brightness while ON
 *   ALARM      DRV wake-up alarm test (15 s buzz pattern)
 *
 * On boot: SD mounts, mic PDM installed but idle. Recording sequences
 * mount SD if not already, open a session-specific file, drain to disk.
 *
 * Ship mode: double-click GPIO16 any time. If a recording is in progress
 * the file is flushed / closed (WAV header patched) before BATFET drops.
 */

#ifndef FIELD_CAPTURE_H
#define FIELD_CAPTURE_H


// Driver version: MAJOR.MINOR.PATCH -- bump PATCH on any change here,
// MINOR on feature adds, MAJOR on release quality (beta / RC / GA).
#define FIELD_CAPTURE_DRIVER_VERSION  "0.2.0"
#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    FCM_MIC = 0,
    FCM_ENV,
    FCM_MOTION,
    FCM_SKIN,
    FCM_FLASHLIGHT,
    FCM_ALARM,
    FCM_COMPASS,   // ESP-IDF-only addition (not in 7_demo_field_capture sketch):
                   // 10 s figure-8 hard-iron cal on first press, then compass
                   // heading with red/blue N/S gradient and DRV pulse on lock.
    FCM_ECG,       // ESP-IDF-only addition: LSM6DSV16X Qvar electrostatic
                   // sensing between two body electrodes. Streams raw samples
                   // in Arduino-Serial-Plotter-compatible format and fires DRV
                   // on each detected beat.
    FCM_TEMP,      // Stage 7 addition: aggregates every onboard temperature
                   // sensor (TMP117, BME688, LSM die, MAX30101 die, ESP32-S3
                   // SoC) into one 1 Hz thermal-map printout. Foundation for
                   // future graphical heat-map overlay on the display.
    FCM_BCG,       // Stage 8 addition: ballistocardiography via LSM6DSV16X
                   // accel Z-axis. Bandpass 1-15 Hz, peak-detect with 400 ms
                   // refractory. DRV pulse on each beat, yellow<->red LED.
                   // Best-effort HR while worn tightly and still (rest / sleep).
    FCM_COUNT,
} fc_mode_t;

/**
 * @brief Initialise NVS state (current mode, boot_seq) and best-effort mount
 *        the SD card. Safe to call multiple times; no-op after first.
 */
void field_capture_init(void);

/**
 * @brief The task that owns encoder / button polling + state machine.
 *        Pinned to Core 1 in boot_tasks.c. Never returns.
 */
void task_field_capture_fn(void *arg);

#ifdef __cplusplus
}
#endif

#endif // FIELD_CAPTURE_H
