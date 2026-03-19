/*
 * ui.c – 3×3 grid of fill-bar sliders with individual decay-rate selectors
 *
 * Nine cells are evenly spread in a 3×3 grid.  Each cell shows:
 *   • A zone label at the top of the cell.
 *   • A horizontal fill-bar slider whose indicator colour reflects the value.
 *     The slider is draggable left/right.  The knob is invisible so only the
 *     coloured fill bar is shown.  A shimmer stripe sweeps across the bar in
 *     the fill direction, creating a "charging battery" wave effect.
 *   • Three rectangular buttons (80×50 px) labeled 0 / 1 / 2 that select how
 *     quickly the bar returns to zero automatically:
 *       0 – no automatic decrease
 *       1 – decrease by 1 unit every 2 seconds
 *       2 – decrease by 1 unit every second
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
#define LEFT_STRIP_W       40

/* Slider bar dimensions */
#define SLIDER_H            60    /* bar height in pixels                        */
#define SLIDER_W_SWEEP     260    /* estimated max slider width (≥ actual)       */

/* Shimmer ("charging battery" wave) – timer-driven, clipped to fill extent */
#define SHIMMER_W          36    /* width of the sweeping stripe (pixels)       */
#define SHIMMER_TICK_MS    40    /* timer interval in ms (~25 fps)              */
#define SHIMMER_PX_PER_TICK  5  /* pixels advanced per tick (~125 px/s)        */

/* Rectangular selector button */
#define SQBTN_W            80    /* button width  in pixels                     */
#define SQBTN_H            50    /* button height in pixels (reduced for label) */

/* Shared label colour for selector buttons */
#define SQBTN_LABEL_R     220
#define SQBTN_LABEL_G     220
#define SQBTN_LABEL_B     220

/* Zone name per cell index (row-major order, 0–8) */
static const char *s_zone_names[BAR_COUNT] = {
    "Zone 1", "Zone 2", "Zone 3",   /* row 0: top,    left → right */
    "Zone 3", "Zone 2", "Zone 1",   /* row 1: middle, left → right */
    "Zone 1", "Zone 2", "Zone 3",   /* row 2: bottom, left → right */
};

/*
 * Vertical text for the left strip.
 * Each character is placed on its own line so the label fits in the narrow
 * width without requiring LVGL transform rotation (which does not affect the
 * layout bounding box and would be clipped by the parent container).
 * Row order top → bottom: 3rd Level, 2nd Level, 1st Level.
 */
static const char *s_level_vtext[GRID_ROWS] = {
    "3\nr\nd\n \nL\ne\nv\ne\nl",   /* 3rd Level */
    "2\nn\nd\n \nL\ne\nv\ne\nl",   /* 2nd Level */
    "1\ns\nt\n \nL\ne\nv\ne\nl",   /* 1st Level */
};

/* ── per-cell state ─────────────────────────────────────────────────── */
typedef struct {
    lv_obj_t *slider;        /* horizontal fill-bar slider */
    lv_obj_t *radio[3];      /* radio buttons for decay rates 0, 1, 2 */
    int32_t   value;
    int       decay_rate;    /* 0 = none, 1 = −1/2 s, 2 = −1/s */
    lv_obj_t *shimmer_clip;  /* clipping container over the fill region */
    lv_obj_t *shimmer_stripe;/* the sweeping stripe inside the clip */
    int32_t   shimmer_x;     /* current stripe x in clip-relative pixels */
    bool      shimmer_rtl;   /* true for RTL (middle row) bars */
} bar_cell_t;

static bar_cell_t  s_cells[BAR_COUNT];
static lv_timer_t *s_flash_timer   = NULL;
static bool        s_flash_state   = false;
static lv_timer_t *s_decay_timer   = NULL;
static uint32_t    s_decay_tick    = 0;
static lv_timer_t *s_shimmer_timer = NULL;

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

/* ── shimmer / wave animation ────────────────────────────────────────── */

/*
 * Periodic timer that advances every shimmer stripe by SHIMMER_PX_PER_TICK
 * pixels in the fill direction, clipped to the current fill extent so the
 * wave never extends beyond the highlighted portion of the bar.
 */
static void shimmer_timer_cb(lv_timer_t *t)
{
    (void)t;
    for (int i = 0; i < BAR_COUNT; i++) {
        int32_t sw = lv_obj_get_width(s_cells[i].slider);
        if (sw <= 0) continue;

        int32_t fill_w = (int32_t)((int64_t)sw * s_cells[i].value / VALUE_MAX);

        /* Resize/reposition the clip container to cover only the fill region */
        if (s_cells[i].shimmer_rtl) {
            lv_obj_set_pos(s_cells[i].shimmer_clip, sw - fill_w, 0);
        } else {
            lv_obj_set_pos(s_cells[i].shimmer_clip, 0, 0);
        }
        lv_obj_set_size(s_cells[i].shimmer_clip, fill_w, SLIDER_H);

        if (fill_w <= 0) continue;

        /* Advance the stripe and wrap when it exits the fill area */
        if (s_cells[i].shimmer_rtl) {
            s_cells[i].shimmer_x -= SHIMMER_PX_PER_TICK;
            if (s_cells[i].shimmer_x < -(int32_t)SHIMMER_W) {
                s_cells[i].shimmer_x = fill_w;   /* re-enter from right edge */
            }
        } else {
            s_cells[i].shimmer_x += SHIMMER_PX_PER_TICK;
            if (s_cells[i].shimmer_x > fill_w) {
                s_cells[i].shimmer_x = -(int32_t)SHIMMER_W; /* re-enter from left */
            }
        }
        lv_obj_set_x(s_cells[i].shimmer_stripe, s_cells[i].shimmer_x);
    }
}

/*
 * Create the shimmer clip container and stripe for cell idx.
 * The clip container is a child of the slider and is resized each timer tick
 * to cover only the filled portion, clipping the stripe to that region.
 * rtl=true → fill grows right-to-left (middle row).
 * idx is used to stagger initial stripe positions across bars.
 */
static void add_shimmer(lv_obj_t *slider, int idx, bool rtl)
{
    /* Clip container – positioned and sized over the fill region (initially 0 wide) */
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

    /* Shimmer stripe – child of clip, so it is clipped to the fill region */
    lv_obj_t *sh = lv_obj_create(clip);
    lv_obj_add_flag(sh, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_clear_flag(sh, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(sh, SHIMMER_W, SLIDER_H);
    lv_obj_set_style_bg_color(sh, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(sh, LV_OPA_30, 0);
    lv_obj_set_style_border_width(sh, 0, 0);
    lv_obj_set_style_radius(sh, 0, 0);
    lv_obj_set_style_pad_all(sh, 0, 0);

    /* Stagger initial x so bars don't all pulse in sync */
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
    lv_obj_set_style_bg_color(scr, bg_blue(), 0);

    /* Row background colours – three distinct blue shades, one per level */
    lv_color_t row_bg_colors[GRID_ROWS];
    row_bg_colors[0] = lv_color_make(20,  45, 100);   /* 3rd Level – deep navy   */
    row_bg_colors[1] = lv_color_make(20,  75, 135);   /* 2nd Level – mid blue    */
    row_bg_colors[2] = lv_color_make(18, 110, 165);   /* 1st Level – steel blue  */

    lv_color_t row_border_colors[GRID_ROWS];
    row_border_colors[0] = lv_color_make(50,  75, 135);
    row_border_colors[1] = lv_color_make(45, 105, 165);
    row_border_colors[2] = lv_color_make(40, 140, 195);

    /* ── Left strip: LEFT_STRIP_W px wide, full height, one coloured band per level ── */
    lv_obj_t *left_strip = lv_obj_create(scr);
    lv_obj_set_size(left_strip, LEFT_STRIP_W, LVGL_PORT_V_RES);
    lv_obj_set_pos(left_strip, 0, 0);
    lv_obj_set_style_bg_opa(left_strip, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(left_strip, 0, 0);
    lv_obj_set_style_pad_all(left_strip, 0, 0);
    lv_obj_clear_flag(left_strip, LV_OBJ_FLAG_SCROLLABLE);
    /* No layout – children are positioned manually */

    lv_coord_t seg_h = LVGL_PORT_V_RES / GRID_ROWS;   /* one band per level row */
    for (int row_idx = 0; row_idx < GRID_ROWS; row_idx++) {
        lv_obj_t *seg = lv_obj_create(left_strip);
        lv_obj_add_flag(seg, LV_OBJ_FLAG_IGNORE_LAYOUT);
        lv_obj_clear_flag(seg, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_pos(seg, 0, row_idx * seg_h);
        lv_obj_set_size(seg, LEFT_STRIP_W, seg_h);
        lv_obj_set_style_bg_color(seg, row_bg_colors[row_idx], 0);
        lv_obj_set_style_bg_opa(seg, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(seg, 0, 0);
        lv_obj_set_style_pad_all(seg, 0, 0);
        lv_obj_set_style_radius(seg, 0, 0);

        lv_obj_t *lvl_lbl = lv_label_create(seg);
        lv_label_set_text(lvl_lbl, s_level_vtext[row_idx]);
        lv_obj_set_style_text_color(lvl_lbl, lv_color_white(), 0);
        lv_obj_set_style_text_align(lvl_lbl, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_center(lvl_lbl);
    }

    /* ── 3×3 grid container (shifted right by LEFT_STRIP_W) ────────── */
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
        lv_obj_set_style_bg_color(cell, row_bg_colors[row], 0);
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

        /* ── Zone label at top of cell ─────────────────────────────── */
        lv_obj_t *zone_lbl = lv_label_create(cell);
        lv_label_set_text(zone_lbl, s_zone_names[i]);
        lv_obj_set_style_text_color(zone_lbl, lv_color_white(), 0);
        lv_obj_set_style_text_align(zone_lbl, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(zone_lbl, lv_pct(100));

        /* ── Horizontal fill-bar slider ────────────────────────────── */
        lv_obj_t *slider = lv_slider_create(cell);
        lv_obj_set_size(slider, lv_pct(100), SLIDER_H);
        lv_slider_set_range(slider, 0, VALUE_MAX);
        lv_slider_set_value(slider, 0, LV_ANIM_OFF);
        /* Track (empty portion) */
        lv_obj_set_style_bg_color(slider, lv_color_make(50, 50, 70), LV_PART_MAIN);
        lv_obj_set_style_radius(slider, 4, LV_PART_MAIN);
        lv_obj_set_style_border_color(slider, lv_color_make(80, 80, 110), LV_PART_MAIN);
        lv_obj_set_style_border_width(slider, 1, LV_PART_MAIN);
        lv_obj_set_style_pad_all(slider, 0, LV_PART_MAIN);
        /* Indicator (fill portion) */
        lv_obj_set_style_bg_color(slider, value_to_color(0), LV_PART_INDICATOR);
        lv_obj_set_style_radius(slider, 4, LV_PART_INDICATOR);
        /* Knob – made invisible so only the fill bar is visible */
        lv_obj_set_style_opa(slider, LV_OPA_TRANSP, LV_PART_KNOB);
        lv_obj_set_style_pad_all(slider, 0, LV_PART_KNOB);
        /* Mirror fill direction for middle row (indices 3–5) */
        if (row == 1) {
            lv_obj_set_style_base_dir(slider, LV_BASE_DIR_RTL, 0);
        }
        lv_obj_add_event_cb(slider, slider_changed_cb, LV_EVENT_VALUE_CHANGED,
                            (void *)(intptr_t)i);
        s_cells[i].slider = slider;

        /* Shimmer stripe – "charging battery" wave sweeping in fill direction */
        add_shimmer(slider, i, row == 1);

        /* ── Radio-button row (decay rate 0 / 1 / 2) ─────────────── */
        lv_obj_t *radio_row = lv_obj_create(cell);
        lv_obj_set_size(radio_row, lv_pct(100), LV_SIZE_CONTENT);
        lv_obj_set_style_bg_opa(radio_row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(radio_row, 0, 0);
        lv_obj_set_style_pad_all(radio_row, 2, 0);
        lv_obj_set_style_pad_column(radio_row, 2, 0);
        lv_obj_clear_flag(radio_row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_layout(radio_row, LV_LAYOUT_FLEX);
        lv_obj_set_flex_flow(radio_row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(radio_row,
                              LV_FLEX_ALIGN_SPACE_EVENLY,
                              LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);

        for (int r = 0; r < 3; r++) {
            /* Rectangular button (SQBTN_W × SQBTN_H) with label centered inside */
            lv_obj_t *rb = lv_obj_create(radio_row);
            lv_obj_set_size(rb, SQBTN_W, SQBTN_H);
            lv_obj_set_style_radius(rb, 0, LV_PART_MAIN);
            lv_obj_set_style_bg_color(rb, lv_color_make(50, 50, 70),
                                      LV_PART_MAIN);
            lv_obj_set_style_bg_color(rb, lv_color_make(100, 200, 100),
                                      LV_PART_MAIN | LV_STATE_CHECKED);
            lv_obj_set_style_border_color(rb, lv_color_make(120, 120, 150),
                                          LV_PART_MAIN);
            lv_obj_set_style_border_width(rb, 2, LV_PART_MAIN);
            lv_obj_add_flag(rb, LV_OBJ_FLAG_CHECKABLE);
            lv_obj_clear_flag(rb, LV_OBJ_FLAG_SCROLLABLE);

            lv_obj_t *lbl = lv_label_create(rb);
            lv_label_set_text(lbl, radio_labels[r]);
            lv_obj_set_style_text_color(lbl,
                lv_color_make(SQBTN_LABEL_R, SQBTN_LABEL_G, SQBTN_LABEL_B), 0);
            lv_obj_center(lbl);

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

    /* ── Shimmer timer: advances the wave stripes at ~25 fps ────── */
    s_shimmer_timer = lv_timer_create(shimmer_timer_cb, SHIMMER_TICK_MS, NULL);
}
