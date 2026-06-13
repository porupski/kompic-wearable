/**
 * @file data_broker.c
 * @brief Central data broker — macro-based implementation.
 *
 * BROKER_MODULE_IMPL(name, NAME, timeout_ms, has_enable) expands to all 8
 * standard functions. Modules with non-standard status logic get a manual
 * get_status() override instead (GPS, MAG, battery). (Blueprint 4 §4-5)
 *
 * To add a new module: one BROKER_MODULE_IMPL line here + one hw flag below.
 * Nothing else in this file changes.
 *
 * Phase 15: g_ui_event_q created in broker_init() — the Core 0 → Core 1
 * command queue. See ui_event.h for contract.
 */

#include "data_broker.h"
#include "ui_event.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <string.h>

static const char *TAG = "BROKER";

// --- Atomic globals ---------------------------------------------------------

volatile bool     g_gps_time_seeded       = false;
volatile uint8_t  g_saved_brightness      = 70;
// NOTE: g_screen_locked, g_wake_display, g_show_shutdown_overlay,
//       g_shutdown_latched, g_dtap_screen_off defined in boot_power.c
volatile uint8_t  g_ui_theme              = UI_THEME_DARK;
volatile bool     g_blue_light_on         = false;

// IMU gesture event (stub -- no IMU driver yet)
volatile uint8_t  g_imu_gesture           = 0;  // imu_gesture_t value
// NOTE: g_tz_offset_hours is defined in ui_broker.c (Core 1 only, NVS-loaded)

// --- UI Event Queue (Core 0 -> Core 1 commands) -----------------------------

QueueHandle_t g_ui_event_q = NULL;

#define UI_EVENT_QUEUE_DEPTH  8

// --- Internal broker state --------------------------------------------------

typedef struct {
    SemaphoreHandle_t    mutex;
    broker_gps_data_t     gps;
    broker_mag_data_t     mag;
    broker_rtc_data_t     rtc;
    broker_battery_data_t battery;
    broker_light_data_t   light;
    broker_haptic_data_t  haptic;
    broker_imu_data_t     imu;
    broker_fusion_data_t  fusion;
    broker_env_data_t     env;
    broker_hr_data_t      hr;
    broker_skin_data_t    skin;
    broker_alarm_data_t   alarm;
    // -- ADD NEW MODULE SLOT HERE ---------------------------------------------
} broker_state_t;

static broker_state_t s = {0};

// --- HW alive flags ---------------------------------------------------------
// Written once at boot by broker_xxx_set_hw_status() -- before any task starts.
// Read-only forever after. No mutex. No volatile needed (write before xTaskCreate).

static struct {
    bool gps, mag, rtc, battery, light;
    bool imu, env, hr, haptic, skin;
    bool fusion;
    bool alarm;
    // -- ADD NEW MODULE FLAG HERE ---------------------------------------------
} hw = {0};

// --- Helpers ----------------------------------------------------------------

static inline uint32_t now_ms(void) {
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}
static inline bool lock(uint32_t timeout_ms) {
    return xSemaphoreTake(s.mutex, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}
static inline void unlock(void) {
    xSemaphoreGive(s.mutex);
}

// --- Init -------------------------------------------------------------------

void broker_init(void) {
    memset(&s, 0, sizeof(s));
    s.mutex = xSemaphoreCreateMutex();
    configASSERT(s.mutex);

    // UI Event Queue -- Core 0 produces, Core 1 consumes.
    // Must exist before any task starts. Depth 8 x sizeof(ui_event_t).
    g_ui_event_q = xQueueCreate(UI_EVENT_QUEUE_DEPTH, sizeof(ui_event_t));
    configASSERT(g_ui_event_q);

    ESP_LOGI(TAG, "Broker init OK (ui_event_q depth=%d, item=%u bytes)",
             UI_EVENT_QUEUE_DEPTH, (unsigned)sizeof(ui_event_t));
}

// --- BROKER_MODULE_IMPL macro -----------------------------------------------
//
// Expands to 8 functions for standard modules:
//   write, read, get_status, set_enabled, get_enabled, set_hw_status, hw_alive
//
// has_enable = true  -> sensor can be toggled on/off by UI (GPS, MAG, light)
// has_enable = false -> always-on (battery, RTC); set_enabled is a no-op
//
// Custom get_status() overrides below REPLACE the macro-generated version for
// GPS, MAG, and battery -- do not call the macro for those modules.

#define BROKER_MODULE_IMPL(name, NAME, timeout_ms, has_enable)                \
                                                                               \
void broker_##name##_write(const broker_##name##_data_t *data) {              \
    if (!lock(10)) { ESP_LOGW(TAG, #name "_write: mutex timeout"); return; }  \
    memcpy(&s.name, data, sizeof(broker_##name##_data_t));                    \
    s.name.last_update_ms = now_ms();                                         \
    unlock();                                                                  \
}                                                                              \
                                                                               \
void broker_##name##_read(broker_##name##_data_t *out) {                      \
    if (!lock(10)) { ESP_LOGW(TAG, #name "_read: mutex timeout"); return; }   \
    memcpy(out, &s.name, sizeof(broker_##name##_data_t));                     \
    unlock();                                                                  \
}                                                                              \
                                                                               \
sensor_status_t broker_##name##_get_status(void) {                            \
    if (!lock(5)) return SENSOR_OFFLINE;                                       \
    if (!hw.name)                      { unlock(); return SENSOR_OFFLINE;  }  \
    if (has_enable && !s.name.enabled) { unlock(); return SENSOR_DISABLED; }  \
    uint32_t age = now_ms() - s.name.last_update_ms;                          \
    sensor_status_t st = (age > (uint32_t)(timeout_ms))                       \
                          ? SENSOR_STALE : SENSOR_ONLINE;                     \
    unlock(); return st;                                                       \
}                                                                              \
                                                                               \
void broker_##name##_set_enabled(bool en) {                                   \
    if (!has_enable) return;                                                   \
    if (!lock(10)) return;                                                     \
    s.name.enabled = en;                                                       \
    unlock();                                                                  \
}                                                                              \
                                                                               \
bool broker_##name##_get_enabled(void) {                                      \
    if (!lock(5)) return false;                                                \
    bool v = (has_enable) ? s.name.enabled : true;                            \
    unlock(); return v;                                                        \
}                                                                              \
                                                                               \
void broker_##name##_set_hw_status(bool ok) {                                 \
    hw.name = ok;                                                              \
    ESP_LOGI(TAG, #NAME " hw_alive: %s", ok ? "YES" : "NO");                 \
}                                                                              \
bool broker_##name##_hw_alive(void) { return hw.name; }

// --- Standard module registrations ------------------------------------------
// GPS, MAG, battery: write + read + set/get_enabled + hw_status generated here.
// Their get_status() is overridden manually below.

#define BROKER_MODULE_NO_STATUS(name, NAME, has_enable)                        \
                                                                               \
void broker_##name##_write(const broker_##name##_data_t *data) {              \
    if (!lock(10)) { ESP_LOGW(TAG, #name "_write: mutex timeout"); return; }  \
    memcpy(&s.name, data, sizeof(broker_##name##_data_t));                    \
    s.name.last_update_ms = now_ms();                                         \
    unlock();                                                                  \
}                                                                              \
void broker_##name##_read(broker_##name##_data_t *out) {                      \
    if (!lock(10)) { ESP_LOGW(TAG, #name "_read: mutex timeout"); return; }   \
    memcpy(out, &s.name, sizeof(broker_##name##_data_t));                     \
    unlock();                                                                  \
}                                                                              \
void broker_##name##_set_enabled(bool en) {                                   \
    if (!has_enable) return;                                                   \
    if (!lock(10)) return;                                                     \
    s.name.enabled = en; unlock();                                             \
}                                                                              \
bool broker_##name##_get_enabled(void) {                                      \
    if (!lock(5)) return false;                                                \
    bool v = (has_enable) ? s.name.enabled : true;                            \
    unlock(); return v;                                                        \
}                                                                              \
void broker_##name##_set_hw_status(bool ok) {                                 \
    hw.name = ok;                                                              \
    ESP_LOGI(TAG, #NAME " hw_alive: %s", ok ? "YES" : "NO");                 \
}                                                                              \
bool broker_##name##_hw_alive(void) { return hw.name; }

// Modules using standard macro (get_status = fresh-or-stale):
BROKER_MODULE_IMPL(rtc,    RTC,    BROKER_RTC_TIMEOUT_MS,    false)  // always-on
BROKER_MODULE_IMPL(light,  LIGHT,  BROKER_LIGHT_TIMEOUT_MS,  true)   // toggleable
BROKER_MODULE_IMPL(haptic, HAPTIC, BROKER_HAPTIC_TIMEOUT_MS, true)
BROKER_MODULE_IMPL(imu,    IMU,    BROKER_IMU_TIMEOUT_MS,    true)
BROKER_MODULE_IMPL(env,    ENV,    BROKER_ENV_TIMEOUT_MS,    true)
BROKER_MODULE_IMPL(hr,     HR,     BROKER_HR_TIMEOUT_MS,     true)
BROKER_MODULE_IMPL(skin,   SKIN,   BROKER_SKIN_TIMEOUT_MS,   true)
// -- ADD NEW STANDARD MODULE HERE ---------------------------------------------

// Modules with custom get_status (write/read/enable generated, status manual):
BROKER_MODULE_NO_STATUS(gps,     GPS,     true)   // fix-aware custom status
BROKER_MODULE_NO_STATUS(mag,     MAG,     true)   // calibration-aware custom status
BROKER_MODULE_NO_STATUS(battery, BATTERY, false)  // NOTIF at <10% custom status

BROKER_MODULE_IMPL(fusion, FUSION, BROKER_FUSION_TIMEOUT_MS, false)
BROKER_MODULE_IMPL(alarm, ALARM, BROKER_ALARM_TIMEOUT_MS, false)  // always-on, no user toggle
// --- Custom get_status() overrides ------------------------------------------

// GPS: ACQUIRING until fix, NOTIF on first fix, STALE on timeout (Blueprint 4 §5)
sensor_status_t broker_gps_get_status(void) {
    if (!lock(5)) return SENSOR_OFFLINE;
    if (!hw.gps)         { unlock(); return SENSOR_OFFLINE;  }
    if (!s.gps.enabled)  { unlock(); return SENSOR_DISABLED; }

    uint32_t age = now_ms() - s.gps.last_update_ms;
    if (age > BROKER_GPS_TIMEOUT_MS) { unlock(); return SENSOR_STALE; }

    sensor_status_t st;
    if      (s.gps.first_fix_notified)       st = SENSOR_NOTIF;
    else if (s.gps.fix != GPS_FIX_NONE)      st = SENSOR_ONLINE;
    else                                     st = SENSOR_ACQUIRING;
    unlock();
    return st;
}

// MAG: ACQUIRING when calibrating or uncalibrated (Blueprint 4 §5)
sensor_status_t broker_mag_get_status(void) {
    if (!lock(5)) return SENSOR_OFFLINE;
    if (!hw.mag)         { unlock(); return SENSOR_OFFLINE;  }
    if (!s.mag.enabled)  { unlock(); return SENSOR_DISABLED; }

    uint32_t age = now_ms() - s.mag.last_update_ms;
    if (age > BROKER_MAG_TIMEOUT_MS) { unlock(); return SENSOR_STALE; }

    sensor_status_t st;
    if      (s.mag.calibrating) st = SENSOR_ACQUIRING;
    else if (s.mag.calibrated)  st = SENSOR_ONLINE;
    else                        st = SENSOR_ACQUIRING;
    unlock();
    return st;
}
static bool g_mag_calibrating = false;

void broker_mag_set_calibrating(bool calibrating) {
    g_mag_calibrating = calibrating;
}

bool broker_mag_get_calibrating(void) {
    return g_mag_calibrating;
}

// Battery: NOTIF below 10%, OFFLINE on timeout (Blueprint 4 §5)
sensor_status_t broker_battery_get_status(void) {
    if (!lock(5)) return SENSOR_OFFLINE;
    if (!hw.battery) { unlock(); return SENSOR_OFFLINE; }

    uint32_t age = now_ms() - s.battery.last_update_ms;
    sensor_status_t st;
    if      (age > BROKER_BATTERY_TIMEOUT_MS) st = SENSOR_OFFLINE;
    else if (s.battery.percentage < 10)       st = SENSOR_NOTIF;
    else                                      st = SENSOR_ONLINE;
    unlock();
    return st;
}