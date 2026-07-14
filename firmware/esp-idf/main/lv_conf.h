/**
 * @file lv_conf.h
 * @brief LVGL v9 configuration for ESP32-S3 smartwatch.
 *
 * Blueprint 11 reference — set in stone. Do not edit without updating the blueprint.
 *
 * Key decisions documented here:
 *
 *   LV_USE_FLOAT_PRINTF 1
 *     Without this, any %f / %.Nf in lv_label_set_text_fmt() silently prints
 *     nothing or a bare "f". ALWAYS keep this 1. If flash is critically tight,
 *     set to 0 and apply the snprintf workaround from Blueprint 5 §format_rule
 *     to every affected tile — do NOT leave it 0 without the workaround in place.
 *
 *   Montserrat 8–30 all enabled
 *     Each size costs 3–8 KB flash. Total ~80 KB for the full set.
 *     Disable individual sizes here if flash budget is exceeded, but keep
 *     the sizes actually used in ui_theme_colors.h (UI_FONT_*) always enabled.
 *     Currently in use: 10, 12, 16. Others available for future tiles.
 *
 *   LV_COLOR_16_SWAP 1
 *     ST7789 connected via SPI requires byte-swapped RGB565.
 *     Never disable without checking the display driver first.
 *
 *   LV_MEM_SIZE 64KB
 *     LVGL internal heap: widget trees, style caches, animation state.
 *     Does NOT include frame buffers (those are PSRAM, allocated separately).
 *     Increase to 96KB if LVGL allocation failures appear in logs.
 */

#define LV_CONF_INCLUDE_SIMPLE 1
#ifndef LV_CONF_H
#define LV_CONF_H

#include <stdint.h>

/*====================
   COLOR SETTINGS
 *====================*/
#define LV_COLOR_DEPTH      16
#define LV_COLOR_16_SWAP    1   // Required for ST7789 SPI — do not disable

/*====================
   MEMORY SETTINGS
 *====================*/
// LVGL internal heap — widget trees, styles, animations.
// Frame buffers are allocated separately in PSRAM (MALLOC_CAP_SPIRAM).
#define LV_MEM_SIZE         (64 * 1024U)

/*====================
   HAL SETTINGS
 *====================*/
#define LV_TICK_CUSTOM      1
#define LV_DPI_DEF          130

/*====================
   FONT: MONTSERRAT
   All sizes 8–30 enabled.
   ui_theme_colors.h uses: 10 (CHIP), 12 (LABEL/VALUE), 16 (TITLE).
   Disable unused sizes only under flash pressure.
 *====================*/
#define LV_FONT_MONTSERRAT_8    1
#define LV_FONT_MONTSERRAT_10   1
#define LV_FONT_MONTSERRAT_12   1
#define LV_FONT_MONTSERRAT_14   1
#define LV_FONT_MONTSERRAT_16   1
#define LV_FONT_MONTSERRAT_18   1
#define LV_FONT_MONTSERRAT_20   1
#define LV_FONT_MONTSERRAT_22   1
#define LV_FONT_MONTSERRAT_24   1
#define LV_FONT_MONTSERRAT_26   1
#define LV_FONT_MONTSERRAT_28   1
#define LV_FONT_MONTSERRAT_30   1

/*====================
   SPRINTF / FLOAT
 *====================*/
#define LV_SPRINTF_CUSTOM       0   // Use LVGL's built-in lv_snprintf

// MUST be 1. Without this, lv_label_set_text_fmt("%f") prints "f" or nothing.
// See Blueprint 5 §format_rule for the mandatory snprintf fallback if ever set to 0.
#define LV_USE_FLOAT_PRINTF     1

/*====================
   WIDGETS
 *====================*/
#define LV_USE_BTN              1
#define LV_USE_LABEL            1
#define LV_USE_IMG              1
#define LV_USE_SLIDER           1
#define LV_USE_LIST             1
#define LV_USE_SWITCH           1
#define LV_USE_ARC              1

/*====================
   EXTRA WIDGETS
 *====================*/
#define LV_USE_TILEVIEW         1
#define LV_USE_MSGBOX           1
#define LV_USE_SPINNER          1

/*====================
   THEMES
 *====================*/
#define LV_USE_THEME_DEFAULT    1

/*====================
   LAYOUTS
 *====================*/
#define LV_USE_FLEX             1
#define LV_USE_GRID             0

/*====================
   OTHERS
 *====================*/
#define LV_USE_USER_DATA        1
#define LV_ENABLE_GLOBAL_CUSTOM 0

/*====================
   LOG SETTINGS
 *====================*/
#define LV_USE_LOG              1
#if LV_USE_LOG
    #define LV_LOG_LEVEL        LV_LOG_LEVEL_WARN
    #define LV_LOG_PRINTF       0
#endif

/*====================
   DEMOS — keep off for production
 *====================*/
#define LV_USE_DEMO_WIDGETS     0

#endif /* LV_CONF_H */