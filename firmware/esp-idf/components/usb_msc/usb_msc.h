/**
 * @file usb_msc.h
 * @brief Lazy USB Mass Storage Class over SDMMC for iv7.1.
 *
 * Safety story (see Stage 11 handoff notes on TinyUSB):
 *
 *   TinyUSB and USB-Serial-JTAG share the ESP32-S3 USB PHY. Once TinyUSB
 *   installs, the JTAG monitor / esptool reset path over USB-Serial-JTAG is
 *   gone until the next reset. iv7.1 has no exposed BOOT button, so the only
 *   escape when TinyUSB is loaded is a hardware reset (BQ25619 QON via
 *   ship-mode gesture, USB unplug + long press, or full power removal).
 *
 *   For that reason:
 *     - This component is initialised LAZILY: nothing runs at boot.
 *     - On enter, we take VFS ownership away from sdcard.c, do a fresh
 *       SDMMC card probe, hand it to tinyusb_msc_storage_init_sdmmc,
 *       install TinyUSB, and drop the WS2812 into cyan.
 *     - The component NEVER attempts to uninstall TinyUSB (the API is
 *       fragile). Instead, on any exit trigger (button click, USB unplug),
 *       we call esp_restart(). Next boot is clean: no TinyUSB active,
 *       USB-Serial-JTAG monitor works, esptool flashes as usual.
 *
 * Call from field_capture's FCM_USB_MSC mode handler only.
 */

#ifndef USB_MSC_H
#define USB_MSC_H

// Driver version: MAJOR.MINOR.PATCH -- bump PATCH on any change here,
// MINOR on feature adds, MAJOR on release quality (beta / RC / GA).
#define USB_MSC_DRIVER_VERSION  "0.2.0"

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Enter USB MSC mode. Blocks until either (a) `button_pressed_cb`
 *        returns true, or (b) USB is unplugged (tud_connected() edge
 *        false-after-true). On any exit condition, calls esp_restart()
 *        and never returns. On setup failure, returns without rebooting.
 *
 * The button poll callback is invoked from the loop tick (~50 ms). Wire it
 * to the same encoder/button poll that field_capture uses so a single click
 * exits.
 *
 * @param button_pressed_cb  Callback returning true when the user wants out.
 *                           Must be non-blocking. May be NULL, in which case
 *                           the loop exits only on USB unplug.
 * @return ESP_OK is never returned (the function reboots on success). On
 *         setup failure, the error from the first failing step is returned
 *         and control returns to the caller.
 */
esp_err_t usb_msc_run_until_exit(bool (*button_pressed_cb)(void));

#ifdef __cplusplus
}
#endif

#endif // USB_MSC_H
