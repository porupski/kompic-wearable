/**
 * @file lvgl_ui_screenshot.h
 * @brief Capture the active LVGL screen to a PNG on SD.
 *
 * Stage 23 §1.3 -- bench visual verification tool for the pre-Mk1b window.
 * Runs even in LVGL_FORCE ON (no-panel) mode because lv_snapshot_take()
 * renders into its own draw buffer, not the flush cb.
 *
 * Encoding: 466x466 RGB888, PNG with uncompressed deflate stored blocks
 * (no zlib dep). File is ~640 KB. Filename via pcf85063_cmd_filename_stamp
 * -> `kompic_YYYY-MM-DD_HH-MM-SS_screenshot.png`; falls back to uptime-ms
 * suffix if the RTC is invalid.
 *
 * SD mount policy: this function mounts if not already mounted and unmounts
 * on exit. Safe to call from any core; the CLI verb runs on the CLI task.
 */
#ifndef LVGL_UI_SCREENSHOT_H
#define LVGL_UI_SCREENSHOT_H

#include <stddef.h>
#include "esp_err.h"

// Capture the current active LVGL screen (lv_scr_act()) into a PNG on SD.
// out_path (>= 128 chars recommended) receives the on-disk path so the CLI
// verb can echo it. Pass NULL if you don't need the path.
esp_err_t lvgl_ui_screenshot_write(char *out_path, size_t out_path_len);

#endif // LVGL_UI_SCREENSHOT_H
