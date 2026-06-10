/**
 * @file boot_power.c
 * @brief Power primitives -- GPIO16 button (BQ25619 QON), GPIO0 (DRV_EN), PSRAM verify.
 *
 * v7.2 redesign (replaces the GPIO40 / GPIO41 polling logic carried over
 * from the v5 latched-power design):
 *   - No GPIO power latch (GPIO41 is now flashlight, Phase 4).
 *   - Button moved from GPIO40 (polled) to GPIO16 (ISR + hold timing).
 *   - Long-hold calls bq25619_enter_ship_mode() instead of dropping a GPIO.
 *   - GPIO0 driven LOW so DRV2605 stays enabled (strap is pull-up by default).
 *
 * Button behaviour (consumed by Core 1 LVGL):
 *   Short press while AWAKE  -> g_display_sleep = true  (+ haptic double-click)
 *   Short press while ASLEEP -> g_wake_display  = true
 *   Hold >= 2.5 s            -> g_show_shutdown_overlay = true (visual warning)
 *   Hold >= 3.0 s            -> bq25619_enter_ship_mode() + g_shutdown_latched
 *
 * No lock screen. Display sleep is the only intra-session power state.
 */

#include "boot_power.h"
#include "bq25619.h"
#include "data_broker.h"
#include "haptic.h"
#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// Forward declare -- avoids including ui_lock_screen.h from boot_power
extern bool display_is_asleep(void);

static const char *TAG = "BOOT_POWER";

volatile bool g_wake_display          = false;
volatile bool g_display_sleep         = false;
volatile bool g_show_shutdown_overlay = false;
volatile bool g_shutdown_latched      = false;

// Press classification thresholds live in boot_power.h.

// ---------------------------------------------------------------------------
// boot_power_init -- runs as the first line of app_main()
// ---------------------------------------------------------------------------
void boot_power_init(void)
{
    // 1. DRV_EN strap LOW -- DRV2605 stays out of shutdown.
    //    GPIO0 has an internal pull-up at boot for ROM strapping; we drive it
    //    LOW immediately after boot so the haptic IC is enabled by the time
    //    drv2605_init() runs.
    gpio_config_t drv_en_cfg = {
        .pin_bit_mask = 1ULL << GPIO_DRV_EN,
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&drv_en_cfg);
    gpio_set_level(GPIO_DRV_EN, 0);
    ESP_LOGI(TAG, "DRV_EN driven LOW (GPIO%d)", GPIO_DRV_EN);

    // 2. Power button input. INT install is deferred to task_power_btn_fn so
    //    the ISR's task path is ready before the first edge can fire.
    gpio_config_t btn_cfg = {
        .pin_bit_mask = 1ULL << GPIO_PWR_BTN,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,    // idles HIGH; press = LOW
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,     // ISR installed later
    };
    gpio_config(&btn_cfg);

    // 3. PSRAM sanity. Display + LVGL need it; fail loudly if absent.
    size_t psram_total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    if (psram_total < (1024 * 1024)) {
        ESP_LOGE(TAG, "PSRAM unavailable or < 1 MB!");
    } else {
        ESP_LOGI(TAG, "PSRAM: %u KB total", (unsigned)(psram_total / 1024));
    }
}

// ---------------------------------------------------------------------------
// GPIO16 ISR -- minimal: notify the button task and return.
// ---------------------------------------------------------------------------
static TaskHandle_t s_btn_task_hdl = NULL;

static void IRAM_ATTR power_btn_isr(void *arg)
{
    (void)arg;
    BaseType_t hpw = pdFALSE;
    if (s_btn_task_hdl) {
        vTaskNotifyGiveFromISR(s_btn_task_hdl, &hpw);
    }
    if (hpw) portYIELD_FROM_ISR();
}

// ---------------------------------------------------------------------------
// Button task -- waits on ISR notify; tracks edges with esp_timer_get_time();
// classifies short / overlay / ship-mode.
//
// We flip the ISR's edge sense each notification:
//   - In idle (button up), ISR is configured NEGEDGE -- next press wakes us.
//   - After a press is detected, we reconfigure to POSEDGE so the release
//     wakes us with the hold duration to classify.
//
// vTaskDelay + gpio_get_level polling inside the task body is also used as a
// fallback for hold-duration detection (so we don't depend solely on edge
// events for the >=3 s threshold).
// ---------------------------------------------------------------------------
static esp_err_t install_isr(gpio_int_type_t edge)
{
    esp_err_t svc = gpio_install_isr_service(ESP_INTR_FLAG_LEVEL1);
    if (svc != ESP_OK && svc != ESP_ERR_INVALID_STATE) return svc;

    gpio_set_intr_type(GPIO_PWR_BTN, edge);
    esp_err_t ret = gpio_isr_handler_add(GPIO_PWR_BTN, power_btn_isr, NULL);
    if (ret == ESP_ERR_INVALID_STATE) {
        // already added -- benign
        ret = ESP_OK;
    }
    gpio_intr_enable(GPIO_PWR_BTN);
    return ret;
}

void task_power_btn_fn(void *arg)
{
    (void)arg;
    s_btn_task_hdl = xTaskGetCurrentTaskHandle();

    // Settle the supply before the first edge counts.
    vTaskDelay(pdMS_TO_TICKS(500));

    if (install_isr(GPIO_INTR_NEGEDGE) != ESP_OK) {
        ESP_LOGE(TAG, "ISR install failed -- button is dead");
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "Power button armed on GPIO%d (ISR-driven, neg edge)", GPIO_PWR_BTN);

    while (1) {
        // Wait for any edge.
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        // Debounce.
        vTaskDelay(pdMS_TO_TICKS(15));
        if (gpio_get_level(GPIO_PWR_BTN) != 0) {
            // glitch / bounce; re-arm for next falling edge
            install_isr(GPIO_INTR_NEGEDGE);
            continue;
        }

        // Press detected. Time it.
        const int64_t t_press = esp_timer_get_time();
        bool ship_triggered = false;

        // Watch for release OR ship-mode threshold, whichever comes first.
        // 50 ms polling is a reasonable compromise -- coarse enough not to
        // burn CPU, fine enough that "exactly 3 s" feels deterministic.
        for (;;) {
            vTaskDelay(pdMS_TO_TICKS(50));
            const bool down  = (gpio_get_level(GPIO_PWR_BTN) == 0);
            const uint32_t held_ms = (uint32_t)((esp_timer_get_time() - t_press) / 1000);

            if (held_ms >= PWR_HOLD_OVERLAY_MS && !g_show_shutdown_overlay) {
                g_show_shutdown_overlay = true;
            }

            if (down && held_ms >= PWR_HOLD_SHIPMODE_MS && !g_shutdown_latched) {
                g_shutdown_latched = true;
                ESP_LOGW(TAG, "Hold %u ms -- entering ship-mode", (unsigned)held_ms);
                vTaskDelay(pdMS_TO_TICKS(200));   // let the overlay paint
                (void)bq25619_enter_ship_mode(I2C_NUM_1);
                ship_triggered = true;
                // BATFET disconnect takes ~10 s; loop forever so we don't
                // process the release as a short press.
                for (;;) vTaskDelay(pdMS_TO_TICKS(1000));
            }

            if (!down) break;
        }

        if (ship_triggered) continue;  // unreachable, but keeps the analyzer happy

        const uint32_t held_ms = (uint32_t)((esp_timer_get_time() - t_press) / 1000);
        g_show_shutdown_overlay = false;

        if (held_ms < PWR_SHORT_PRESS_MAX_MS) {
            if (display_is_asleep()) {
                g_wake_display = true;
                ESP_LOGI(TAG, "Wake display");
            } else {
                g_display_sleep = true;
                haptic_play(HAPTIC_EFFECT_DOUBLE_CLICK);
                ESP_LOGI(TAG, "Sleep display (haptic)");
            }
        } else {
            ESP_LOGI(TAG, "Held %u ms -- not classified (cancelled before ship-mode)",
                     (unsigned)held_ms);
        }

        // Re-arm for next press (negedge again).
        install_isr(GPIO_INTR_NEGEDGE);
    }
}
