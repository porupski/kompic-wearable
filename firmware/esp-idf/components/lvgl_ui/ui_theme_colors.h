/**
 * @file ui_theme_colors.h
 * @brief Central UI palette, style constants, and theme-resolved helpers.
 *
 * RULES:
 *   - Never hardcode lv_color_hex() or lv_palette_main() outside this file.
 *   - Status colours (green, red, yellow...) are theme-independent. Do NOT
 *     invert them on theme change — they carry semantic meaning.
 *   - All size constants are in pixels. One edit here = uniform across all tiles.
 *   - To override a single constant in one tile, undef and redefine locally
 *     AFTER the include:
 *
 *       #include "ui_theme_colors.h"
 *       #undef  COL_BG_DARK
 *       #define COL_BG_DARK  lv_color_hex(0x000000)  // pitch black for this tile only
 *
 * DEPENDENCIES:
 *   - lvgl.h  (for lv_color_t, lv_color_hex, lv_palette_main)
 *   - data_broker.h  (for ui_theme_t, g_ui_theme)
 */

#ifndef UI_THEME_COLORS_H
#define UI_THEME_COLORS_H

#include "lvgl.h"
#include "data_broker.h"  // ui_theme_t, g_ui_theme

// ═══════════════════════════════════════════════════════════════════════════════
//  DARK THEME PALETTE
//  Background: anthracite (near-black grey) — not pure black.
//  Text:       off-white — easier on AMOLED / dim environments.
// ═══════════════════════════════════════════════════════════════════════════════

#define COL_BG_DARK          lv_color_hex(0x1C1C1E)   // Anthracite
#define COL_TEXT_DARK        lv_color_hex(0xF0EDE8)   // Off-white
#define COL_SUBTEXT_DARK     lv_color_hex(0x8E8E93)   // Dimmed label
#define COL_DIVIDER_DARK     lv_color_hex(0x3A3A3C)   // Subtle separator
#define COL_ROW_BG_DARK      lv_color_hex(0x2C2C2E)   // Card / list row background

// ═══════════════════════════════════════════════════════════════════════════════
//  LIGHT THEME PALETTE
//  Background: warm ivory — not pure white.
//  Text:       near-black — not pure black; gentler contrast.
// ═══════════════════════════════════════════════════════════════════════════════

#define COL_BG_LIGHT         lv_color_hex(0xFFFBF0)   // Warm ivory
#define COL_TEXT_LIGHT       lv_color_hex(0x111111)   // Near-black
#define COL_SUBTEXT_LIGHT    lv_color_hex(0x6C6C70)   // Dimmed label
#define COL_DIVIDER_LIGHT    lv_color_hex(0xD0CCC0)   // Warm separator
#define COL_ROW_BG_LIGHT     lv_color_hex(0xF0EBE0)   // Card / list row background

// ═══════════════════════════════════════════════════════════════════════════════
//  THEME-RESOLVED HELPERS
//  Use these in tile .c files instead of branching on g_ui_theme manually.
//  Each helper returns the correct colour for the active theme at call time.
// ═══════════════════════════════════════════════════════════════════════════════

static inline lv_color_t theme_bg(void) {
    return (g_ui_theme == UI_THEME_LIGHT) ? COL_BG_LIGHT      : COL_BG_DARK;
}
static inline lv_color_t theme_text(void) {
    return (g_ui_theme == UI_THEME_LIGHT) ? COL_TEXT_LIGHT     : COL_TEXT_DARK;
}
static inline lv_color_t theme_subtext(void) {
    return (g_ui_theme == UI_THEME_LIGHT) ? COL_SUBTEXT_LIGHT  : COL_SUBTEXT_DARK;
}
static inline lv_color_t theme_divider(void) {
    return (g_ui_theme == UI_THEME_LIGHT) ? COL_DIVIDER_LIGHT  : COL_DIVIDER_DARK;
}
static inline lv_color_t theme_row_bg(void) {
    return (g_ui_theme == UI_THEME_LIGHT) ? COL_ROW_BG_LIGHT   : COL_ROW_BG_DARK;
}

// ═══════════════════════════════════════════════════════════════════════════════
//  STATUS / ACCENT COLOURS
//  Semantic colours — never inverted between themes.
//  Map directly to sensor_status_t values for consistent LED dot rendering.
// ═══════════════════════════════════════════════════════════════════════════════

#define COL_STATUS_DISABLED  lv_palette_main(LV_PALETTE_GREY)    // SENSOR_DISABLED
#define COL_STATUS_OFFLINE   lv_palette_main(LV_PALETTE_RED)     // SENSOR_OFFLINE
#define COL_STATUS_ACQUIRING lv_palette_main(LV_PALETTE_YELLOW)  // SENSOR_ACQUIRING
#define COL_STATUS_STALE     lv_palette_main(LV_PALETTE_ORANGE)  // SENSOR_STALE
#define COL_STATUS_ONLINE    lv_palette_main(LV_PALETTE_GREEN)   // SENSOR_ONLINE
#define COL_STATUS_NOTIF     lv_color_make(180, 0, 255)          // SENSOR_NOTIF (purple)

// Convenience aliases (legacy names kept for tile compat)
#define COL_STATUS_RED       COL_STATUS_OFFLINE
#define COL_STATUS_GREEN     COL_STATUS_ONLINE
#define COL_STATUS_YELLOW    COL_STATUS_ACQUIRING
#define COL_STATUS_ORANGE    COL_STATUS_STALE
#define COL_STATUS_GREY      COL_STATUS_DISABLED
#define COL_STATUS_PURPLE    COL_STATUS_NOTIF

// iOS-style blue — used for interactive elements: buttons, sliders, links
#define COL_ACCENT           lv_color_hex(0x0A84FF)

// ═══════════════════════════════════════════════════════════════════════════════
//  LAYOUT SIZE CONSTANTS
//  Applies uniformly to all tiles unless locally overridden.
// ═══════════════════════════════════════════════════════════════════════════════

#define UI_SWITCH_W          50    // lv_switch width  (px)
#define UI_SWITCH_H          26    // lv_switch height (px)
#define UI_BTN_H             30    // Standard action button height (px)
#define UI_ROW_H             34    // Switch-row container height (px)
#define UI_TILE_PAD_H        10    // Horizontal inner padding per tile (px)
#define UI_TILE_PAD_V         8    // Vertical inner padding per tile (px)
#define UI_DIVIDER_H          1    // Divider line height (px)
#define UI_DIVIDER_W        210    // Divider line width (px)

// ═══════════════════════════════════════════════════════════════════════════════
//  FONT CONSTANTS
//  All tiles use these unless locally overridden.
//  Requires CONFIG_LV_FONT_MONTSERRAT_xx=y in sdkconfig.
// ═══════════════════════════════════════════════════════════════════════════════

#define UI_FONT_TITLE        (&lv_font_montserrat_16)  // Tile header / section title
#define UI_FONT_LABEL        (&lv_font_montserrat_14)  // Field labels
#define UI_FONT_VALUE        (&lv_font_montserrat_12)  // Data values
#define UI_FONT_CHIP         (&lv_font_montserrat_10)  // Chip name / small annotation

#endif // UI_THEME_COLORS_H