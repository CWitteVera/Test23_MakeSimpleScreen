/*
 * ui.c – 3×3 grid of fill-bar sliders driven by MQTT data
 *
 * Nine cells are evenly spread in a 3×3 grid.  Each cell shows:
 *   • A zone label at the top of the cell.
 *   • A horizontal fill-bar (read-only slider) whose indicator colour
 *     reflects the current value.  A shimmer stripe sweeps across the bar
 *     in the fill direction, giving a "charging battery" wave effect.
 *   • An info row with two read-only text boxes:
 *       "Current"  – the latest current count from MQTT
 *       "Total"    – the day total count from MQTT
 *
 * Zone labels per row:
 *   Top row    (left → right): Zone 1, Zone 2, Zone 3
 *   Middle row (left → right): Zone 3, Zone 2, Zone 1
 *   Bottom row (left → right): Zone 1, Zone 2, Zone 3
 *
 * A left strip shows vertical row labels: 3rd Level / 2nd Level / 1st Level.
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
 *
 * app_ui_set_zone(zone_idx, current, day_total) is the only public update API;
 * it is thread-safe and may be called from any FreeRTOS task.
 */

#include "ui.h"
#include "lvgl.h"
#include "lvgl_port.h"

/* ── constants ──────────────────────────────────────────────────────── */
#define BAR_COUNT  9
#define GRID_COLS  3
#define GRID_ROWS  3
#define VALUE_MAX  40

/* Left-strip width for row level labels */
#define LEFT_STRIP_W       60

/* Slider bar dimensions */
#define SLIDER_H            60

/* Shimmer ("charging battery" wave) – timer-driven */
#define SHIMMER_W           36
#define SHIMMER_TICK_MS     40
#define SHIMMER_PX_PER_TICK  5
#define SLIDER_W_SWEEP     260   /* estimated max slider width (>= actual) */

/* Info box dimensions */
#define INFO_BOX_W          110
#define INFO_BOX_H           48

/* Zone name per cell index (row-major order, 0-8) */
static const char *s_zone_names[BAR_COUNT] = {
    "Zone 1", "Zone 2", "Zone 3",   /* row 0: top,    3rd Level */
    "Zone 3", "Zone 2", "Zone 1",   /* row 1: middle, 2nd Level */
    "Zone 1", "Zone 2", "Zone 3",   /* row 2: bottom, 1st Level */
};

static const char *s_level_vtext[GRID_ROWS] = {
    "3rd\nLevel",
    "2nd\nLevel",
    "1st\nLevel",
};

/* ── per-cell state ─────────────────────────────────────────────────── */
typedef struct {
    lv_obj_t *slider;
    lv_obj_t *current_lbl;   /* value label inside the "Current" info box */
    lv_obj_t *total_lbl;     /* value label inside the "Total"   info box */
    int32_t   value;
    lv_obj_t *shimmer_clip;
    lv_obj_t *shimmer_stripe;
    int32_t   shimmer_x;
    bool      shimmer_rtl;
} bar_cell_t;

static bar_cell_t  s_cells[BAR_COUNT];
static lv_timer_t *s_flash_timer   = NULL;
static bool        s_flash_state   = false;
static lv_timer_t *s_shimmer_timer = NULL;
static lv_color_t  s_row_bg_colors[GRID_ROWS];
static lv_obj_t   *s_bg_bands[GRID_ROWS];

/* ── colour helpers ─────────────────────────────────────────────────── */
static lv_color_t value_to_color(int32_t v)
{
    uint8_t r = 0, g = 0, b = 0;

    if (v <= 20) {
        r = 0; g = 200;
    } else if (v <= 25) {
        int t = v - 21;
        r = (uint8_t)(t * 255 / 4); g = 200;
    } else if (v <= 30) {
        r = 255; g = 220;
    } else if (v <= 35) {
        int t = v - 31;
        r = 255; g = (uint8_t)(220 * (4 - t) / 4);
    } else {
        r = 220; g = 0;
    }
    return lv_color_make(r, g, b);
}

/* ── flash timer ────────────────────────────────────────────────────── */
static void flash_cb(lv_timer_t *t)
{
    (void)t;

    bool any_at_max = false;
    for (int i = 0; i < BAR_COUNT; i++) {
        if (s_cells[i].value >= VALUE_MAX) { any_at_max = true; break; }
    }
    if (!any_at_max) {
        for (int r = 0; r < GRID_ROWS; r++)
            lv_obj_set_style_bg_color(s_bg_bands[r], s_row_bg_colors[r], 0);
        lv_timer_del(s_flash_timer);
        s_flash_timer = NULL;
        s_flash_state = false;
        return;
    }

    s_flash_state = !s_flash_state;
    lv_color_t flash_red = lv_color_make(220, 0, 0);
    for (int r = 0; r < GRID_ROWS; r++) {
        lv_color_t c = s_flash_state ? flash_red : s_row_bg_colors[r];
        lv_obj_set_style_bg_color(s_bg_bands[r], c, 0);
    }
}

/* ── update a single cell after its value changes ───────────────────── */
static void update_cell(int idx)
{
    lv_obj_set_style_bg_color(s_cells[idx].slider,
                              value_to_color(s_cells[idx].value),
                              LV_PART_INDICATOR);

    bool any_at_max = false;
    for (int i = 0; i < BAR_COUNT; i++) {
        if (s_cells[i].value >= VALUE_MAX) { any_at_max = true; break; }
    }

    if (any_at_max && s_flash_timer == NULL) {
        s_flash_timer = lv_timer_create(flash_cb, 500, NULL);
        lv_timer_ready(s_flash_timer);
    } else if (!any_at_max && s_flash_timer != NULL) {
        lv_timer_del(s_flash_timer);
        s_flash_timer = NULL;
        s_flash_state = false;
        for (int r = 0; r < GRID_ROWS; r++)
            lv_obj_set_style_bg_color(s_bg_bands[r], s_row_bg_colors[r], 0);
    }
}

/* ── shimmer / wave animation ────────────────────────────────────────── */
static void shimmer_timer_cb(lv_timer_t *t)
{
    (void)t;
    for (int i = 0; i < BAR_COUNT; i++) {
        int32_t sw = lv_obj_get_width(s_cells[i].slider);
        if (sw <= 0) continue;

        int32_t fill_w = (int32_t)((int64_t)sw * s_cells[i].value / VALUE_MAX);

        if (s_cells[i].shimmer_rtl) {
            lv_obj_set_pos(s_cells[i].shimmer_clip, sw - fill_w, 0);
        } else {
            lv_obj_set_pos(s_cells[i].shimmer_clip, 0, 0);
        }
        lv_obj_set_size(s_cells[i].shimmer_clip, fill_w, SLIDER_H);

        if (fill_w <= 0) continue;

        if (s_cells[i].shimmer_rtl) {
            s_cells[i].shimmer_x -= SHIMMER_PX_PER_TICK;
            if (s_cells[i].shimmer_x < -(int32_t)SHIMMER_W)
                s_cells[i].shimmer_x = fill_w;
        } else {
            s_cells[i].shimmer_x += SHIMMER_PX_PER_TICK;
            if (s_cells[i].shimmer_x > fill_w)
                s_cells[i].shimmer_x = -(int32_t)SHIMMER_W;
        }
        lv_obj_set_x(s_cells[i].shimmer_stripe, s_cells[i].shimmer_x);
    }
}

static void add_shimmer(lv_obj_t *slider, int idx, bool rtl)
{
    lv_obj_t *clip = lv_obj_create(slider);
    lv_obj_add_flag(clip, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_clear_flag(clip, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE
                             | LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_set_size(clip, 0, SLIDER_H);
    lv_obj_set_pos(clip, 0, 0);
    lv_obj_set_style_bg_opa(clip, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(clip, 0, 0);
    lv_obj_set_style_radius(clip, 0, 0);
    lv_obj_set_style_pad_all(clip, 0, 0);

    lv_obj_t *sh = lv_obj_create(clip);
    lv_obj_add_flag(sh, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_clear_flag(sh, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(sh, SHIMMER_W, SLIDER_H);
    lv_obj_set_style_bg_color(sh, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(sh, LV_OPA_30, 0);
    lv_obj_set_style_border_width(sh, 0, 0);
    lv_obj_set_style_radius(sh, 0, 0);
    lv_obj_set_style_pad_all(sh, 0, 0);

    int32_t span    = SLIDER_W_SWEEP + SHIMMER_W;
    int32_t stagger = span / BAR_COUNT;
    int32_t init_x  = rtl
        ? SLIDER_W_SWEEP - (int32_t)idx * stagger
        : -(int32_t)SHIMMER_W + (int32_t)idx * stagger;
    lv_obj_set_x(sh, init_x);

    s_cells[idx].shimmer_clip   = clip;
    s_cells[idx].shimmer_stripe = sh;
    s_cells[idx].shimmer_x      = init_x;
    s_cells[idx].shimmer_rtl    = rtl;
}

/* ── public API ──────────────────────────────────────────────────────── */
void app_ui_init(void)
{
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_set_style_border_width(scr, 0, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(scr, lv_color_make(20, 45, 100), 0);

    s_row_bg_colors[0] = lv_color_make(20,  45, 100);
    s_row_bg_colors[1] = lv_color_make(50, 120, 190);
    s_row_bg_colors[2] = lv_color_make(20,  45, 100);

    lv_color_t row_border_colors[GRID_ROWS];
    row_border_colors[0] = lv_color_make(50,  75, 135);
    row_border_colors[1] = lv_color_make(80, 155, 215);
    row_border_colors[2] = lv_color_make(50,  75, 135);

    /* ── Full-width background bands ── */
    lv_coord_t band_h = LVGL_PORT_V_RES / GRID_ROWS;
    for (int r = 0; r < GRID_ROWS; r++) {
        lv_coord_t this_h = (r == GRID_ROWS - 1)
                            ? (LVGL_PORT_V_RES - r * band_h)
                            : band_h;
        lv_obj_t *band = lv_obj_create(scr);
        lv_obj_add_flag(band, LV_OBJ_FLAG_IGNORE_LAYOUT);
        lv_obj_clear_flag(band, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_pos(band, 0, r * band_h);
        lv_obj_set_size(band, LVGL_PORT_H_RES, this_h);
        lv_obj_set_style_bg_color(band, s_row_bg_colors[r], 0);
        lv_obj_set_style_bg_opa(band, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(band, 0, 0);
        lv_obj_set_style_radius(band, 0, 0);
        lv_obj_set_style_pad_all(band, 0, 0);
        s_bg_bands[r] = band;
    }

    /* ── Left strip ── */
    lv_obj_t *left_strip = lv_obj_create(scr);
    lv_obj_set_size(left_strip, LEFT_STRIP_W, LVGL_PORT_V_RES);
    lv_obj_set_pos(left_strip, 0, 0);
    lv_obj_set_style_bg_opa(left_strip, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(left_strip, 0, 0);
    lv_obj_set_style_pad_all(left_strip, 0, 0);
    lv_obj_clear_flag(left_strip, LV_OBJ_FLAG_SCROLLABLE);

    lv_coord_t seg_h = LVGL_PORT_V_RES / GRID_ROWS;
    for (int row_idx = 0; row_idx < GRID_ROWS; row_idx++) {
        lv_obj_t *seg = lv_obj_create(left_strip);
        lv_obj_add_flag(seg, LV_OBJ_FLAG_IGNORE_LAYOUT);
        lv_obj_clear_flag(seg, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_pos(seg, 0, row_idx * seg_h);
        lv_obj_set_size(seg, LEFT_STRIP_W, seg_h);
        lv_obj_set_style_bg_opa(seg, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(seg, 0, 0);
        lv_obj_set_style_pad_all(seg, 0, 0);
        lv_obj_set_style_radius(seg, 0, 0);

        lv_obj_t *lvl_lbl = lv_label_create(seg);
        lv_label_set_text(lvl_lbl, s_level_vtext[row_idx]);
        lv_obj_set_style_text_color(lvl_lbl, lv_color_white(), 0);
        lv_obj_set_style_text_align(lvl_lbl, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(lvl_lbl, LEFT_STRIP_W);
        lv_obj_center(lvl_lbl);
    }

    /* ── 3x3 grid container ── */
    static lv_coord_t col_dsc[] = {
        LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST
    };
    static lv_coord_t row_dsc[] = {
        LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST
    };

    lv_obj_t *grid = lv_obj_create(scr);
    lv_obj_set_size(grid, LVGL_PORT_H_RES - LEFT_STRIP_W, LVGL_PORT_V_RES);
    lv_obj_set_pos(grid, LEFT_STRIP_W, 0);
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

        /* ── Cell container ── */
        lv_obj_t *cell = lv_obj_create(grid);
        lv_obj_set_grid_cell(cell,
                             LV_GRID_ALIGN_STRETCH, col, 1,
                             LV_GRID_ALIGN_STRETCH, row, 1);
        lv_obj_set_style_pad_all(cell, 4, 0);
        lv_obj_set_style_bg_opa(cell, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_color(cell, row_border_colors[row], 0);
        lv_obj_set_style_border_width(cell, 1, 0);
        lv_obj_set_style_radius(cell, 4, 0);
        lv_obj_clear_flag(cell, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_layout(cell, LV_LAYOUT_FLEX);
        lv_obj_set_flex_flow(cell, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(cell,
                              LV_FLEX_ALIGN_SPACE_BETWEEN,
                              LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);

        /* ── Zone label ── */
        lv_obj_t *zone_lbl = lv_label_create(cell);
        lv_label_set_text(zone_lbl, s_zone_names[i]);
        lv_obj_set_style_text_color(zone_lbl, lv_color_white(), 0);
        lv_obj_set_style_text_align(zone_lbl, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(zone_lbl, lv_pct(100));

        /* ── Fill-bar slider (read-only, MQTT-driven) ── */
        lv_obj_t *slider = lv_slider_create(cell);
        lv_obj_set_size(slider, lv_pct(100), SLIDER_H);
        lv_obj_clear_flag(slider, LV_OBJ_FLAG_CLICKABLE);
        lv_slider_set_range(slider, 0, VALUE_MAX);
        lv_slider_set_value(slider, 0, LV_ANIM_OFF);
        lv_obj_set_style_bg_color(slider, lv_color_make(50, 50, 70), LV_PART_MAIN);
        lv_obj_set_style_radius(slider, 4, LV_PART_MAIN);
        lv_obj_set_style_border_color(slider, lv_color_make(80, 80, 110), LV_PART_MAIN);
        lv_obj_set_style_border_width(slider, 1, LV_PART_MAIN);
        lv_obj_set_style_pad_all(slider, 0, LV_PART_MAIN);
        lv_obj_set_style_bg_color(slider, value_to_color(0), LV_PART_INDICATOR);
        lv_obj_set_style_radius(slider, 4, LV_PART_INDICATOR);
        lv_obj_set_style_opa(slider, LV_OPA_TRANSP, LV_PART_KNOB);
        lv_obj_set_style_pad_all(slider, 0, LV_PART_KNOB);
        s_cells[i].slider = slider;

        add_shimmer(slider, i, false);

        /* ── Info row: Current | Total ── */
        lv_obj_t *info_row = lv_obj_create(cell);
        lv_obj_set_size(info_row, lv_pct(100), LV_SIZE_CONTENT);
        lv_obj_set_style_bg_opa(info_row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(info_row, 0, 0);
        lv_obj_set_style_pad_all(info_row, 2, 0);
        lv_obj_set_style_pad_column(info_row, 4, 0);
        lv_obj_clear_flag(info_row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_layout(info_row, LV_LAYOUT_FLEX);
        lv_obj_set_flex_flow(info_row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(info_row,
                              LV_FLEX_ALIGN_SPACE_EVENLY,
                              LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);

        static const char *box_titles[2] = {"Current", "Total"};
        lv_obj_t **val_ptrs[2] = {
            &s_cells[i].current_lbl,
            &s_cells[i].total_lbl
        };

        for (int b = 0; b < 2; b++) {
            lv_obj_t *box = lv_obj_create(info_row);
            lv_obj_set_size(box, INFO_BOX_W, INFO_BOX_H);
            lv_obj_set_style_bg_color(box, lv_color_make(30, 30, 55), 0);
            lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
            lv_obj_set_style_border_color(box, lv_color_make(80, 80, 120), 0);
            lv_obj_set_style_border_width(box, 1, 0);
            lv_obj_set_style_radius(box, 4, 0);
            lv_obj_set_style_pad_all(box, 2, 0);
            lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_layout(box, LV_LAYOUT_FLEX);
            lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
            lv_obj_set_flex_align(box,
                                  LV_FLEX_ALIGN_CENTER,
                                  LV_FLEX_ALIGN_CENTER,
                                  LV_FLEX_ALIGN_CENTER);

            lv_obj_t *title = lv_label_create(box);
            lv_label_set_text(title, box_titles[b]);
            lv_obj_set_style_text_color(title, lv_color_make(180, 180, 210), 0);
            lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);

            lv_obj_t *val = lv_label_create(box);
            lv_label_set_text(val, "--");
            lv_obj_set_style_text_color(val, lv_color_white(), 0);
            lv_obj_set_style_text_align(val, LV_TEXT_ALIGN_CENTER, 0);
            *val_ptrs[b] = val;
        }
    }

    /* ── Shimmer timer ── */
    s_shimmer_timer = lv_timer_create(shimmer_timer_cb, SHIMMER_TICK_MS, NULL);
}

void app_ui_set_zone(int zone_idx, int current, int day_total)
{
    if (zone_idx < 0 || zone_idx >= BAR_COUNT) return;

    if (lvgl_port_lock(-1)) {
        int32_t bar_val = (int32_t)(current < VALUE_MAX ? current : VALUE_MAX);
        if (bar_val < 0) bar_val = 0;

        s_cells[zone_idx].value = bar_val;
        lv_slider_set_value(s_cells[zone_idx].slider, bar_val, LV_ANIM_OFF);
        update_cell(zone_idx);

        lv_label_set_text_fmt(s_cells[zone_idx].current_lbl, "%d", current);
        lv_label_set_text_fmt(s_cells[zone_idx].total_lbl,   "%d", day_total);

        lvgl_port_unlock();
    }
}
