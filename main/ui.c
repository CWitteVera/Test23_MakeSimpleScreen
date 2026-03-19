/*
 * ui.c – 3×3 grid of fill-bar sliders with individual decay-rate selectors
 *
 * Nine cells are evenly spread in a 3×3 grid.  Each cell shows:
 *   • A horizontal fill-bar slider (twice the previous height) whose indicator
 *     colour reflects the value.  The slider is draggable left/right.
 *     The knob is invisible so only the coloured fill bar is shown.
 *   • Three radio buttons labeled 0 / 1 / 2 that select how quickly the bar
 *     returns to zero automatically:
 *       0 – no automatic decrease
 *       1 – decrease by 1 unit every 2 seconds
 *       2 – decrease by 1 unit every second
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
    lv_obj_t *slider;       /* horizontal fill-bar slider */
    lv_obj_t *radio[3];     /* radio buttons for decay rates 0, 1, 2 */
    int32_t   value;
    int       decay_rate;   /* 0 = none, 1 = −1/2 s, 2 = −1/s */
} bar_cell_t;

static bar_cell_t  s_cells[BAR_COUNT];
static lv_timer_t *s_flash_timer = NULL;
static bool        s_flash_state = false;
static lv_timer_t *s_decay_timer = NULL;
static uint32_t    s_decay_tick  = 0;

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
    update_cell(idx);
}

/* Called when a radio button is tapped – enforces mutual exclusivity */
static void radio_cb(lv_event_t *e)
{
    uint32_t ud = (uint32_t)(uintptr_t)lv_event_get_user_data(e);
    int cell_idx  = (int)(ud >> 16);
    int radio_idx = (int)(ud & 0xFFFFu);

    s_cells[cell_idx].decay_rate = radio_idx;

    /* Ensure exactly the selected button is checked */
    for (int r = 0; r < 3; r++) {
        if (r == radio_idx) {
            lv_obj_add_state(s_cells[cell_idx].radio[r], LV_STATE_CHECKED);
        } else {
            lv_obj_clear_state(s_cells[cell_idx].radio[r], LV_STATE_CHECKED);
        }
    }
}

/* 1-second periodic timer – decrements bar values according to decay rate */
static void decay_cb(lv_timer_t *t)
{
    (void)t;
    s_decay_tick++;

    for (int i = 0; i < BAR_COUNT; i++) {
        if (s_cells[i].value <= 0) continue;

        bool do_dec = false;
        if (s_cells[i].decay_rate == 2) {
            do_dec = true;                          /* −1 every second */
        } else if (s_cells[i].decay_rate == 1 && (s_decay_tick & 1u) == 0u) {
            do_dec = true;                          /* −1 every 2 seconds */
        }

        if (do_dec) {
            s_cells[i].value--;
            if (s_cells[i].value < 0) s_cells[i].value = 0;
            lv_slider_set_value(s_cells[i].slider, s_cells[i].value, LV_ANIM_OFF);
            update_cell(i);
        }
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

    static const char *radio_labels[] = {"0", "1", "2"};

    for (int i = 0; i < BAR_COUNT; i++) {
        int col = i % GRID_COLS;
        int row = i / GRID_COLS;

        s_cells[i].value      = 0;
        s_cells[i].decay_rate = 0;

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

        /* ── Horizontal fill-bar slider (twice the previous height) ─── */
        lv_obj_t *slider = lv_slider_create(cell);
        lv_obj_set_size(slider, lv_pct(100), 60);
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

        /* ── Radio-button row (decay rate 0 / 1 / 2) ─────────────── */
        lv_obj_t *radio_row = lv_obj_create(cell);
        lv_obj_set_size(radio_row, lv_pct(100), LV_SIZE_CONTENT);
        lv_obj_set_style_bg_opa(radio_row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(radio_row, 0, 0);
        lv_obj_set_style_pad_all(radio_row, 2, 0);
        lv_obj_set_style_pad_column(radio_row, 6, 0);
        lv_obj_clear_flag(radio_row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_layout(radio_row, LV_LAYOUT_FLEX);
        lv_obj_set_flex_flow(radio_row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(radio_row,
                              LV_FLEX_ALIGN_SPACE_EVENLY,
                              LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);

        for (int r = 0; r < 3; r++) {
            lv_obj_t *rb = lv_checkbox_create(radio_row);
            lv_checkbox_set_text(rb, radio_labels[r]);
            /* Style indicator as a radio button (circular) */
            lv_obj_set_style_radius(rb, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
            lv_obj_set_style_bg_color(rb, lv_color_make(50, 50, 70),
                                      LV_PART_INDICATOR);
            lv_obj_set_style_bg_color(rb, lv_color_make(100, 200, 100),
                                      LV_PART_INDICATOR | LV_STATE_CHECKED);
            lv_obj_set_style_border_color(rb, lv_color_make(120, 120, 150),
                                          LV_PART_INDICATOR);
            lv_obj_set_style_border_width(rb, 1, LV_PART_INDICATOR);
            lv_obj_set_style_text_color(rb, lv_color_make(200, 200, 200), 0);
            /* Rate 0 is selected by default */
            if (r == 0) {
                lv_obj_add_state(rb, LV_STATE_CHECKED);
            }
            uint32_t ud = ((uint32_t)i << 16) | (uint32_t)r;
            lv_obj_add_event_cb(rb, radio_cb, LV_EVENT_VALUE_CHANGED,
                                (void *)(uintptr_t)ud);
            s_cells[i].radio[r] = rb;
        }
    }

    /* ── Decay timer: fires every 1 second ────────────────────────── */
    s_decay_timer = lv_timer_create(decay_cb, 1000, NULL);
}
