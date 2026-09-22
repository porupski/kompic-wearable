/**
 * @file ui_pane_styles.c
 * @brief Shared LVGL styles for pane roots, drawers, rows, and cards.
 *
 * See ui_pane_styles.h for API contract + threading rules.
 * See LVGL9_Kompic_Architecture.md §2.2 + §5.3 for design rationale.
 */

#include "ui_pane_styles.h"
#include "ui_theme_colors.h"
#include "data_broker.h"       // g_ui_theme, UI_THEME_LIGHT
#include "esp_log.h"
#include <stdbool.h>

static const char *TAG = "PANE_STYLE";

// ---------------------------------------------------------------------------
// Module-static style singletons
// ---------------------------------------------------------------------------

static lv_style_t s_style_pane_root;
static lv_style_t s_style_pane_drawer;
static lv_style_t s_style_row;
static lv_style_t s_style_card;
static lv_style_t s_style_subtext;
static bool       s_initialised = false;

lv_style_t *kw_ui_style_pane_root(void)   { return &s_style_pane_root;   }
lv_style_t *kw_ui_style_pane_drawer(void) { return &s_style_pane_drawer; }
lv_style_t *kw_ui_style_row(void)         { return &s_style_row;         }
lv_style_t *kw_ui_style_card(void)        { return &s_style_card;        }
lv_style_t *kw_ui_style_subtext(void)     { return &s_style_subtext;     }

// ---------------------------------------------------------------------------
// Internal -- theme-tracked property writes
// ---------------------------------------------------------------------------

static void write_theme_props(void)
{
    lv_color_t bg  = theme_bg();
    lv_color_t txt = theme_text();
    lv_color_t row = theme_row_bg();

    ESP_LOGD(TAG, "write_theme_props: theme=%s bg=0x%06lX txt=0x%06lX row=0x%06lX",
             g_ui_theme == UI_THEME_LIGHT ? "LIGHT" : "DARK",
             (unsigned long)lv_color_to_u32(bg),
             (unsigned long)lv_color_to_u32(txt),
             (unsigned long)lv_color_to_u32(row));

    lv_style_set_bg_color(&s_style_pane_root,   bg);
    lv_style_set_text_color(&s_style_pane_root, txt);

    lv_style_set_bg_color(&s_style_pane_drawer,   bg);
    lv_style_set_text_color(&s_style_pane_drawer, txt);

    lv_style_set_bg_color(&s_style_row,   row);
    lv_style_set_text_color(&s_style_row, txt);

    lv_style_set_bg_color(&s_style_card,   row);
    lv_style_set_text_color(&s_style_card, txt);

    lv_style_set_text_color(&s_style_subtext, theme_subtext());
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void kw_ui_pane_styles_init(void)
{
    if (s_initialised) {
        ESP_LOGD(TAG, "init: already initialised -- no-op");
        return;
    }

    lv_style_init(&s_style_pane_root);
    lv_style_init(&s_style_pane_drawer);
    lv_style_init(&s_style_row);
    lv_style_init(&s_style_card);
    lv_style_init(&s_style_subtext);

    // ── pane_root -- full-screen root container. No border, no radius,
    //     covers the full display. Theme colours applied below.
    lv_style_set_bg_opa(&s_style_pane_root, LV_OPA_COVER);
    lv_style_set_border_width(&s_style_pane_root, 0);
    lv_style_set_radius(&s_style_pane_root, 0);
    lv_style_set_pad_all(&s_style_pane_root, 0);

    // ── pane_drawer -- drawer variant. Rounded corners for the
    //     slide-in-over-home affordance (Stage 31.3 will use this).
    lv_style_set_bg_opa(&s_style_pane_drawer, LV_OPA_COVER);
    lv_style_set_border_width(&s_style_pane_drawer, 0);
    lv_style_set_radius(&s_style_pane_drawer, 12);
    lv_style_set_pad_all(&s_style_pane_drawer, 0);

    // ── row -- horizontal flex row with space-between + centre-Y.
    //     Standard row background + padding for switch rows / list rows.
    lv_style_set_bg_opa(&s_style_row, LV_OPA_COVER);
    lv_style_set_border_width(&s_style_row, 0);
    lv_style_set_radius(&s_style_row, 8);
    lv_style_set_pad_hor(&s_style_row, UI_TILE_PAD_H);
    lv_style_set_pad_ver(&s_style_row, 6);
    lv_style_set_layout(&s_style_row, LV_LAYOUT_FLEX);
    lv_style_set_flex_flow(&s_style_row, LV_FLEX_FLOW_ROW);
    lv_style_set_flex_main_place(&s_style_row, LV_FLEX_ALIGN_SPACE_BETWEEN);
    lv_style_set_flex_cross_place(&s_style_row, LV_FLEX_ALIGN_CENTER);

    // ── card -- rounded card container. Row-bg colour, inner padding
    //     so children don't hug the edge. No flex layout -- caller picks.
    lv_style_set_bg_opa(&s_style_card, LV_OPA_COVER);
    lv_style_set_border_width(&s_style_card, 0);
    lv_style_set_radius(&s_style_card, 12);
    lv_style_set_pad_all(&s_style_card, 12);

    // ── subtext -- text-colour-only style for dim labels. Add to a
    //     label with lv_obj_add_style() to render theme_subtext without
    //     hardcoding a colour or needing an apply_theme hook.
    //     (No bg / no layout props on purpose.)

    write_theme_props();

    s_initialised = true;
    ESP_LOGI(TAG, "init: 5 pane styles created (root/drawer/row/card/subtext)");
}

void kw_ui_pane_styles_reapply_theme(void)
{
    if (!s_initialised) {
        ESP_LOGW(TAG, "reapply_theme: not initialised -- forwarding to init");
        kw_ui_pane_styles_init();
        return;
    }

    write_theme_props();

    lv_obj_t *scr = lv_screen_active();
    if (scr) {
        lv_obj_invalidate(scr);
        ESP_LOGV(TAG, "reapply_theme: invalidated active screen %p", scr);
    } else {
        ESP_LOGW(TAG, "reapply_theme: no active screen to invalidate");
    }

    ESP_LOGI(TAG, "reapply_theme: theme=%s -- 5 styles updated, screen invalidated",
             g_ui_theme == UI_THEME_LIGHT ? "LIGHT" : "DARK");
}
