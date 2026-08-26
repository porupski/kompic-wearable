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
#define FIELD_CAPTURE_DRIVER_VERSION  "0.3.2"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// -- Mode enum ---------------------------------------------------------------
// Stage 10 restructure: flat "one slot per use-case" list is transitioning
// to "one slot per sensor" with per-sensor submenus. LSM is the first sensor
// to migrate: FCM_LSM is a top-level gateway that owns FCM_MOTION, FCM_BCG,
// FCM_STEPS, and FCM_MLC_COLLECT as its submenu entries. The top-level
// encoder cycle SKIPS submenu-only entries (see is_lsm_submode in .c). Other
// sensors stay flat during transition. See project memory:
// [[project-mode-restructure]]. New enum values are appended (not inserted)
// so NVS-stored numeric mode indices from older firmware still decode.
typedef enum {
    FCM_MIC = 0,
    FCM_ENV,
    FCM_MOTION,      // LSM submenu entry (no longer top-level after Stage 10)
    FCM_PPG_BCG,     // Stage 17: repurposed FCM_SKIN slot -- combined PPG (MAX30101
                     // multi-LED raw) + BCG (LSM6DSV16X accel Z) at 200 Hz with
                     // pc_sync_* CSV headers per Stage 15. Enum ordinal preserved
                     // for NVS boot-mode compatibility. Old skin behaviour deleted.
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
                   // SoC) into one 1 Hz thermal-map printout. Stage 10 visual
                   // upgraded to fire strobing (rgb_temp_fire_strobe).
    FCM_BCG,       // LSM submenu entry (no longer top-level after Stage 10).
                   // Ballistocardiography via LSM6DSV16X accel Z-axis.
    // -- Stage 10 additions ------------------------------------------------
    FCM_LSM,           // Top-level gateway to LSM submenu (yellow slot).
    FCM_STEPS,         // LSM submenu: embedded pedometer step counter.
    FCM_MLC_COLLECT,   // LSM submenu: raw accel+gyro CSV for MLC training (S5 stub).
    FCM_TAP_DBG,       // LSM submenu: host-side magnitude-based tap detector
                       // + chip-tap event echo + orientation gate diagnostics.
                       // Used to prove tap subsystem end-to-end at the bench.
    // -- Stage 11 additions ------------------------------------------------
    FCM_USB_MSC,       // Cyan top-level tile. Click -> unmount SD, install
                       // TinyUSB MSC, host sees a removable drive. Second
                       // click or USB unplug -> esp_restart() (fresh state).
                       // See components/usb_msc/ for the bricking-risk story.
    FCM_COUNT,
} fc_mode_t;

// -- LSM submenu order (used by encoder-scroll while in submenu) -------------
// Not an enum -- explicit array. Order = presentation order.
extern const fc_mode_t FC_LSM_SUBMENU[];
extern const uint8_t   FC_LSM_SUBMENU_LEN;

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

/**
 * @brief Read the current-boot sequence number that field_capture loaded from
 *        NVS on startup. Returns 0 before field_capture_init() has run. Used
 *        by the RTC CLI to stamp NVS SET_TIME records with a coherent boot_seq.
 */
uint32_t field_capture_get_boot_seq(void);

#ifdef __cplusplus
}
#endif

#endif // FIELD_CAPTURE_H
