/**
 * @file usb_msc.c
 * @brief Implementation of the lazy USB MSC entry -- see usb_msc.h.
 */

#include "usb_msc.h"

#include <string.h>

#include "esp_log.h"
#include "esp_err.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_pm.h"           // PM lock so USB timing stays clean under DFS
#include "esp_vfs_fat.h"      // esp_vfs_fat_mount_config_t for MSC storage

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/sdmmc_host.h"
#include "sdmmc_cmd.h"

#include "tinyusb.h"
#include "tusb_msc_storage.h"

#include "sdcard.h"
#include "ws2812.h"

static const char *TAG = "USB_MSC";

// -- LED palette (matches MODE_INFO["usb"] in field_capture.c) ------------------
#define WS_MAX      26
#define CYAN_R      0
#define CYAN_G      WS_MAX
#define CYAN_B      WS_MAX

// -- Card handle owned by this component while active ---------------------------
// Heap-allocated because sdmmc_host expects a pointer that outlives init.
static sdmmc_card_t *s_card = NULL;

// Track USB connection state -- fires exit on the true→false edge.
static volatile bool s_was_connected = false;

// True-then-false edge detection on MSC storage ownership. tinyusb_msc's
// mount_changed callback fires with is_mounted=false at registration time
// (before the host has ever mounted), so treating any false as "eject"
// caused the device to reboot the instant TinyUSB installed. We now require
// at least one is_mounted=true event before a subsequent false counts as
// an eject/exit signal.
static volatile bool s_host_was_mounted = false;
static volatile bool s_host_ejected     = false;

// ─── Callbacks ────────────────────────────────────────────────────────────────

static void storage_mount_changed_cb(tinyusb_msc_event_t *event)
{
    if (event == NULL) return;
    bool mounted = event->mount_changed_data.is_mounted;
    ESP_LOGI(TAG, "MSC host mount_changed -> %s (was_mounted=%d)",
             mounted ? "MOUNTED" : "EJECTED", s_host_was_mounted ? 1 : 0);
    if (mounted) {
        s_host_was_mounted = true;
    } else if (s_host_was_mounted) {
        s_host_ejected = true;
    }
    // First "unmounted" event before any mount is ignored -- that is the
    // storage backend telling us its own initial VFS state, not the host.
}

// ─── LED breathing (0.5 Hz cyan pulse while waiting for host) ─────────────────

static void led_cyan_pulse_tick(uint32_t base_ms)
{
    // Simple triangle wave 0..1..0 over 2 s.
    uint32_t t = (uint32_t)(esp_timer_get_time() / 1000LL) - base_ms;
    uint32_t phase = t % 2000;
    uint32_t bright = (phase < 1000) ? phase : (2000 - phase);   // 0..1000
    uint8_t r = (uint8_t)((uint32_t)CYAN_R * bright / 1000);
    uint8_t g = (uint8_t)((uint32_t)CYAN_G * bright / 1000);
    uint8_t b = (uint8_t)((uint32_t)CYAN_B * bright / 1000);
    ws2812_set_color(r, g, b);
}

// ─── Fresh SDMMC probe (independent of sdcard.c's VFS mount) ───────────────────

static esp_err_t probe_sdmmc_card(void)
{
    if (s_card != NULL) return ESP_OK;

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;

    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.clk   = SDCARD_GPIO_CLK;
    slot.cmd   = SDCARD_GPIO_CMD;
    slot.d0    = SDCARD_GPIO_DAT0;
    slot.width = 1;
    slot.flags = SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    esp_err_t err = sdmmc_host_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "sdmmc_host_init failed: %s", esp_err_to_name(err));
        return err;
    }
    err = sdmmc_host_init_slot(SDMMC_HOST_SLOT_1, (const sdmmc_slot_config_t *)&slot);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "sdmmc_host_init_slot failed: %s", esp_err_to_name(err));
        return err;
    }

    s_card = calloc(1, sizeof(sdmmc_card_t));
    if (s_card == NULL) return ESP_ERR_NO_MEM;

    err = sdmmc_card_init(&host, s_card);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "sdmmc_card_init failed: %s", esp_err_to_name(err));
        free(s_card);
        s_card = NULL;
        return err;
    }

    ESP_LOGI(TAG, "Card probed: name=%s, %llu MiB",
             s_card->cid.name,
             ((uint64_t)s_card->csd.capacity * s_card->csd.sector_size) >> 20);
    return ESP_OK;
}

// ─── Public entry ─────────────────────────────────────────────────────────────

esp_err_t usb_msc_run_until_exit(bool (*button_pressed_cb)(void))
{
    ESP_LOGW(TAG, "Entering USB MSC mode. USB-Serial-JTAG monitor will be lost");
    ESP_LOGW(TAG, "until reboot. Click button or unplug USB to exit + reboot.");

    // 0. Hold APB at MAX for the entire MSC session (0.4.13 fix). Without
    // this, DFS may drop APB while TinyUSB is servicing a host request,
    // corrupting the response ("Errno 71 Protocol error", 0-block Read
    // Capacity). Lock is leaked at esp_restart() below -- OK, cleanup is
    // automatic on reset. Non-fatal if lock creation fails.
#ifdef CONFIG_PM_ENABLE
    static esp_pm_lock_handle_t s_msc_pm = NULL;
    if (s_msc_pm == NULL) {
        (void)esp_pm_lock_create(ESP_PM_APB_FREQ_MAX, 0, "usb_msc", &s_msc_pm);
    }
    if (s_msc_pm) {
        (void)esp_pm_lock_acquire(s_msc_pm);
        ESP_LOGI(TAG, "PM lock acquired -- APB pinned at MAX for USB stability");
    }
#endif

    // 1. Release the VFS ownership of the card so we own it directly.
    esp_err_t err = sdcard_unmount();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "sdcard_unmount returned %s (continuing)", esp_err_to_name(err));
    }
    ESP_LOGI(TAG, "SD unmounted from VFS");

    // 2. Fresh SDMMC probe.
    err = probe_sdmmc_card();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Card probe failed -- USB MSC aborted.");
        ws2812_set_color(WS_MAX, 0, 0);
        vTaskDelay(pdMS_TO_TICKS(1000));
        ws2812_set_color(0, 0, 0);
        return err;
    }

    // Sanity check the card -- if capacity is 0 the MSC layer will report
    // 0 blocks to the host and enumeration will fail. Bail early with a
    // visible LED cue.
    if (s_card == NULL || s_card->csd.capacity == 0) {
        ESP_LOGE(TAG, "Card probed but capacity=0 -- MSC would fail. Aborting.");
        ws2812_set_color(WS_MAX, 0, 0);
        vTaskDelay(pdMS_TO_TICKS(1500));
        return ESP_ERR_INVALID_STATE;
    }
    ESP_LOGI(TAG, "Card verified: capacity=%llu MiB, sector_size=%u",
             ((uint64_t)s_card->csd.capacity * s_card->csd.sector_size) >> 20,
             (unsigned)s_card->csd.sector_size);

    // 3. Init the MSC storage backend against our probed card.
    //
    // 0.4.13 fix: mount_config MUST be initialised. Prior versions left it
    // as {0}, which passed max_files=0 / allocation_unit_size=0 down into
    // the FAT premount layer. On some cards / IDF versions the storage
    // backend then reported 0 blocks to the host ("Read Capacity(10)
    // failed: 0 512-byte logical blocks").
    const tinyusb_msc_sdmmc_config_t msc_cfg = {
        .card                          = s_card,
        .callback_mount_changed        = NULL,   // registered explicitly below
        .callback_premount_changed     = NULL,
        .mount_config = (esp_vfs_fat_mount_config_t){
            .format_if_mount_failed   = false,
            .max_files                = 4,
            .allocation_unit_size     = 16U * 1024U,
            .disk_status_check_enable = false,
        },
    };
    err = tinyusb_msc_storage_init_sdmmc(&msc_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "tinyusb_msc_storage_init_sdmmc: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "MSC storage backend initialised");
    tinyusb_msc_register_callback(TINYUSB_MSC_EVENT_MOUNT_CHANGED,
                                    storage_mount_changed_cb);

    // 4. Install TinyUSB. Uses default descriptor -- MSC-only (CDC not built).
    const tinyusb_config_t tusb_cfg = {
        .device_descriptor         = NULL,
        .string_descriptor         = NULL,
        .external_phy              = false,
        .configuration_descriptor  = NULL,
    };
    err = tinyusb_driver_install(&tusb_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "tinyusb_driver_install: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "TinyUSB MSC installed. Waiting for host enumeration...");

    // 5. Loop: cyan pulse + exit polling.
    s_was_connected    = false;
    s_host_was_mounted = false;
    s_host_ejected     = false;
    uint32_t base_ms   = (uint32_t)(esp_timer_get_time() / 1000LL);

    // Grace period: give TinyUSB a few seconds to spin up + host to enumerate
    // before we start looking for exit signals. The PHY handoff from
    // USB-Serial-JTAG to TinyUSB can briefly flip tud_connected() true then
    // false; without this delay we would treat that blip as an unplug and
    // reboot instantly.
    const uint32_t GRACE_MS = 3000;

    // Also require the "not connected" reading to be stable for at least
    // this long before we accept it as an unplug -- protects against
    // transient dropouts during enumeration.
    const uint32_t UNPLUG_STABLE_MS = 500;
    uint32_t last_connected_ms = base_ms;

    for (;;) {
        uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000LL);
        led_cyan_pulse_tick(base_ms);

        bool connected = tud_connected();
        if (connected) {
            s_was_connected   = true;
            last_connected_ms = now_ms;
        }

        // All exit paths are gated by the grace period.
        if ((now_ms - base_ms) >= GRACE_MS) {

            if (s_was_connected && !connected &&
                (now_ms - last_connected_ms) >= UNPLUG_STABLE_MS) {
                ESP_LOGW(TAG, "USB unplug detected -- rebooting.");
                break;
            }

            if (s_host_ejected) {
                ESP_LOGW(TAG, "Host ejected -- rebooting.");
                break;
            }

            if (button_pressed_cb && button_pressed_cb()) {
                ESP_LOGW(TAG, "Button pressed -- rebooting.");
                break;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(50));
    }

    // 6. Cyan off, brief red flash to indicate reboot imminent, then restart.
    ws2812_set_color(WS_MAX, 0, 0);
    vTaskDelay(pdMS_TO_TICKS(200));
    ws2812_set_color(0, 0, 0);
    esp_restart();

    return ESP_OK;  // unreachable
}
