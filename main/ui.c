/*
 * ui.c – 3×3 grid of fill-bar sliders with individual brightness inputs
 *
 * Nine cells are evenly spread in a 3×3 grid.  Each cell shows:
 *   • A horizontal fill-bar slider whose indicator colour reflects the value.
 *     The slider is draggable – slide left/right to change the brightness.
 *     The knob is invisible so only the coloured fill bar is shown.
 *   • A spinbox row (−  value  +) for entering a brightness in 0–40.
 *     Both controls stay in sync with each other.
 *
 * Colour mapping (smooth gradient):
 *    0–20  : solid green
 *   21–25  : green → yellow (linear interpolation)
 *   26–30  : solid yellow
 *   31–35  : yellow → red  (linear interpolation)
 *   36–40  : solid red
 *
 * Background is soft cornflower-blue when no bar is at 40.
 * When any bar reaches 40 the background flashes between blue and red at 2 Hz.
 */

#include "ui.h"
#include "lvgl.h"

/* ── constants ──────────────────────────────────────────────────────── */
#define BAR_COUNT  9
#define GRID_COLS  3
#define VALUE_MAX  40

/* ── per-cell state ─────────────────────────────────────────────────── */
typedef struct {
    lv_obj_t *slider;   /* horizontal fill-bar slider */
    lv_obj_t *spinbox;
    int32_t   value;
} bar_cell_t;

static bar_cell_t  s_cells[BAR_COUNT];
static lv_timer_t *s_flash_timer = NULL;
static bool        s_flash_state = false;

/* ── colour helpers ─────────────────────────────────────────────────── */
static lv_color_t bg_blue(void)
{
    return lv_color_make(100, 149, 237);   /* cornflower blue */
}

static lv_color_t value_to_color(int32_t v)
{
    uint8_t r = 0, g = 0, b = 0;

    if (v <= 20) {
        /* solid green */
        r = 0; g = 200;
    } else if (v <= 25) {
        /* green → yellow: ramp red 0→255 over steps 21..25 (t = 0..4) */
        int t = v - 21;
        r = (uint8_t)(t * 255 / 4); g = 200;
    } else if (v <= 30) {
        /* solid yellow */
        r = 255; g = 220;
    } else if (v <= 35) {
        /* yellow → red: ramp green 220→0 over steps 31..35 (t = 0..4) */
        int t = v - 31;
        r = 255; g = (uint8_t)(220 * (4 - t) / 4);
    } else {
        /* solid red */
        r = 220; g = 0;
    }
    return lv_color_make(r, g, b);
}

/* ── flash timer ────────────────────────────────────────────────────── */
static void flash_cb(lv_timer_t *t)
{
    (void)t;

    /* Stop if no bar is at max any more */
    bool any_at_max = false;
    for (int i = 0; i < BAR_COUNT; i++) {
        if (s_cells[i].value >= VALUE_MAX) { any_at_max = true; break; }
    }
    if (!any_at_max) {
        lv_obj_set_style_bg_color(lv_scr_act(), bg_blue(), 0);
        lv_timer_del(s_flash_timer);
        s_flash_timer = NULL;
        s_flash_state = false;
        return;
    }

    s_flash_state = !s_flash_state;
    lv_color_t c = s_flash_state ? lv_color_make(220, 0, 0) : bg_blue();
    lv_obj_set_style_bg_color(lv_scr_act(), c, 0);
}

/* ── update a single cell after its value changes ───────────────────── */
static void update_cell(int idx)
{
    /* Update slider indicator colour to reflect new value */
    lv_obj_set_style_bg_color(s_cells[idx].slider,
                              value_to_color(s_cells[idx].value),
                              LV_PART_INDICATOR);

    bool any_at_max = false;
    for (int i = 0; i < BAR_COUNT; i++) {
        if (s_cells[i].value >= VALUE_MAX) { any_at_max = true; break; }
    }

    if (any_at_max && s_flash_timer == NULL) {
        s_flash_timer = lv_timer_create(flash_cb, 500, NULL);
        lv_timer_ready(s_flash_timer);   /* fire first flash immediately */
    } else if (!any_at_max && s_flash_timer != NULL) {
        lv_timer_del(s_flash_timer);
        s_flash_timer = NULL;
        s_flash_state = false;
        lv_obj_set_style_bg_color(lv_scr_act(), bg_blue(), 0);
    }
}

/* ── event callbacks ─────────────────────────────────────────────────── */

/* Called when the slider is dragged or programmatically changed */
static void slider_changed_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    int32_t new_val = lv_slider_get_value(s_cells[idx].slider);
    if (new_val == s_cells[idx].value) return;   /* already in sync */
    s_cells[idx].value = new_val;
    lv_spinbox_set_value(s_cells[idx].spinbox, new_val);
    update_cell(idx);
}

/* Called when spinbox text value changes (after any increment / decrement) */
static void spinbox_changed_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    int32_t new_val = lv_spinbox_get_value(s_cells[idx].spinbox);
    if (new_val == s_cells[idx].value) return;   /* already in sync */
    s_cells[idx].value = new_val;
    lv_slider_set_value(s_cells[idx].slider, new_val, LV_ANIM_OFF);
    update_cell(idx);
}

static void spinbox_inc_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_SHORT_CLICKED || code == LV_EVENT_LONG_PRESSED_REPEAT) {
        int idx = (int)(intptr_t)lv_event_get_user_data(e);
        lv_spinbox_increment(s_cells[idx].spinbox);
        /* spinbox_changed_cb fires via LV_EVENT_VALUE_CHANGED and syncs everything */
    }
}

static void spinbox_dec_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_SHORT_CLICKED || code == LV_EVENT_LONG_PRESSED_REPEAT) {
        int idx = (int)(intptr_t)lv_event_get_user_data(e);
        lv_spinbox_decrement(s_cells[idx].spinbox);
        /* spinbox_changed_cb fires via LV_EVENT_VALUE_CHANGED and syncs everything */
    }
}

/* ── public API ──────────────────────────────────────────────────────── */
void app_ui_init(void)
{
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_set_style_border_width(scr, 0, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(scr, bg_blue(), 0);

    /* ── 3×3 grid container ────────────────────────────────────────── */
    static lv_coord_t col_dsc[] = {
        LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST
    };
    static lv_coord_t row_dsc[] = {
        LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST
    };

    lv_obj_t *grid = lv_obj_create(scr);
    lv_obj_set_size(grid, 800, 480);
    lv_obj_set_pos(grid, 0, 0);
    lv_obj_set_style_bg_opa(grid, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(grid, 0, 0);
    lv_obj_set_style_pad_all(grid, 4, 0);
    lv_obj_set_style_pad_column(grid, 4, 0);
    lv_obj_set_style_pad_row(grid, 4, 0);
    lv_obj_clear_flag(grid, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_layout(grid, LV_LAYOUT_GRID);
    lv_obj_set_grid_dsc_array(grid, col_dsc, row_dsc);

    for (int i = 0; i < BAR_COUNT; i++) {
        int col = i % GRID_COLS;
        int row = i / GRID_COLS;

        s_cells[i].value = 0;

        /* ── Cell container ───────────────────────────────────────── */
        lv_obj_t *cell = lv_obj_create(grid);
        lv_obj_set_grid_cell(cell,
                             LV_GRID_ALIGN_STRETCH, col, 1,
                             LV_GRID_ALIGN_STRETCH, row, 1);
        lv_obj_set_style_pad_all(cell, 4, 0);
        lv_obj_set_style_bg_color(cell, lv_color_make(30, 30, 50), 0);
        lv_obj_set_style_border_color(cell, lv_color_make(60, 60, 90), 0);
        lv_obj_set_style_border_width(cell, 1, 0);
        lv_obj_set_style_radius(cell, 4, 0);
        lv_obj_clear_flag(cell, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_layout(cell, LV_LAYOUT_FLEX);
        lv_obj_set_flex_flow(cell, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(cell,
                              LV_FLEX_ALIGN_SPACE_BETWEEN,
                              LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);

        /* ── Horizontal fill-bar slider ─────────────────────────────── */
        lv_obj_t *slider = lv_slider_create(cell);
        /* Full width, fixed height renders as a horizontal slider */
        lv_obj_set_size(slider, lv_pct(100), 30);
        lv_slider_set_range(slider, 0, VALUE_MAX);
        lv_slider_set_value(slider, 0, LV_ANIM_OFF);
        /* Track (empty portion) */
        lv_obj_set_style_bg_color(slider, lv_color_make(50, 50, 70), LV_PART_MAIN);
        lv_obj_set_style_radius(slider, 4, LV_PART_MAIN);
        lv_obj_set_style_border_color(slider, lv_color_make(80, 80, 110), LV_PART_MAIN);
        lv_obj_set_style_border_width(slider, 1, LV_PART_MAIN);
        /* Indicator (fill portion) */
        lv_obj_set_style_bg_color(slider, value_to_color(0), LV_PART_INDICATOR);
        lv_obj_set_style_radius(slider, 4, LV_PART_INDICATOR);
        /* Knob – made invisible so only the fill bar is visible */
        lv_obj_set_style_opa(slider, LV_OPA_TRANSP, LV_PART_KNOB);
        lv_obj_set_style_pad_all(slider, 0, LV_PART_KNOB);
        lv_obj_add_event_cb(slider, slider_changed_cb, LV_EVENT_VALUE_CHANGED,
                            (void *)(intptr_t)i);
        s_cells[i].slider = slider;

        /* ── Spinbox row  [−] [value] [+] ────────────────────────── */
        lv_obj_t *row_cont = lv_obj_create(cell);
        lv_obj_set_size(row_cont, lv_pct(100), LV_SIZE_CONTENT);
        lv_obj_set_style_bg_opa(row_cont, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(row_cont, 0, 0);
        lv_obj_set_style_pad_all(row_cont, 2, 0);
        lv_obj_set_style_pad_column(row_cont, 4, 0);
        lv_obj_clear_flag(row_cont, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_layout(row_cont, LV_LAYOUT_FLEX);
        lv_obj_set_flex_flow(row_cont, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row_cont,
                              LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);

        /* − button */
        lv_obj_t *btn_dec = lv_btn_create(row_cont);
        lv_obj_set_size(btn_dec, 34, 34);
        lv_obj_add_event_cb(btn_dec, spinbox_dec_cb, LV_EVENT_ALL,
                            (void *)(intptr_t)i);
        lv_obj_t *lbl_dec = lv_label_create(btn_dec);
        lv_label_set_text(lbl_dec, LV_SYMBOL_MINUS);
        lv_obj_center(lbl_dec);

        /* Spinbox */
        lv_obj_t *sb = lv_spinbox_create(row_cont);
        lv_spinbox_set_range(sb, 0, VALUE_MAX);
        lv_spinbox_set_digit_format(sb, 2, 0);
        lv_spinbox_set_value(sb, 0);
        lv_obj_set_width(sb, 80);
        lv_obj_add_event_cb(sb, spinbox_changed_cb, LV_EVENT_VALUE_CHANGED,
                            (void *)(intptr_t)i);
        s_cells[i].spinbox = sb;

        /* + button */
        lv_obj_t *btn_inc = lv_btn_create(row_cont);
        lv_obj_set_size(btn_inc, 34, 34);
        lv_obj_add_event_cb(btn_inc, spinbox_inc_cb, LV_EVENT_ALL,
                            (void *)(intptr_t)i);
        lv_obj_t *lbl_inc = lv_label_create(btn_inc);
        lv_label_set_text(lbl_inc, LV_SYMBOL_PLUS);
        lv_obj_center(lbl_inc);
    }
}
