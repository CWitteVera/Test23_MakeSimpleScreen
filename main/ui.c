/*
 * ui.c – 3×3 grid of fill-bar sliders driven by MQTT zone counts
 *
 * Nine cells are evenly spread in a 3×3 grid.  Each cell shows:
 *   • A zone label at the top of the cell.
 *   • A horizontal fill-bar slider whose indicator colour reflects the value.
 *     A shimmer stripe sweeps across the bar in the fill direction, giving a
 *     "charging battery" wave effect.
 *   • A numeric count label showing the last received MQTT count for that zone.
 *     Shows "---" (grey) if no update has been received for ≥10 seconds.
 *
 * Zone labels per row:
 *   Top row    (left → right): Zone 1, Zone 2, Zone 3  (3rd Level)
 *   Middle row (left → right): Zone 3, Zone 2, Zone 1  (2nd Level)
 *   Bottom row (left → right): Zone 1, Zone 2, Zone 3  (1st Level)
 *
 * A left strip shows vertical row labels: 3rd Level / 2nd Level / 1st Level.
 *
 * MQTT updates arrive via ui_update_zone_count(level, zone, count).
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
#include "esp_timer.h"
#include "esp_log.h"
#include <inttypes.h>

#define TAG_UI "ui"

/* ── constants ──────────────────────────────────────────────────────── */
#define BAR_COUNT  9
#define GRID_COLS  3
#define GRID_ROWS  3
#define VALUE_MAX  40

/* Status bar at the bottom of the screen */
#define STATUS_BAR_H   26

/* Staleness threshold: 10 seconds in microseconds */
#define STALE_THRESHOLD_US  (10LL * 1000000LL)

/* Left-strip width for row level labels - wide enough to fit "Level" horizontally */
#define LEFT_STRIP_W       60

/* Slider bar dimensions */
#define SLIDER_H            60    /* bar height in pixels                        */
#define SLIDER_W_SWEEP     260    /* estimated max slider width (≥ actual)       */

/* Shimmer ("charging battery" wave) – timer-driven, clipped to fill extent */
#define SHIMMER_W          36    /* width of the sweeping stripe (pixels)       */
#define SHIMMER_TICK_MS    40    /* timer interval in ms (~25 fps)              */
#define SHIMMER_PX_PER_TICK  5  /* pixels advanced per tick (~125 px/s)        */

/* Maximum wait for the LVGL mutex when called from an external task */
#define LVGL_LOCK_TIMEOUT_MS  10

/* Zone name per cell index (row-major order, 0–8) */
static const char *s_zone_names[BAR_COUNT] = {
    "Zone 1", "Zone 2", "Zone 3",   /* row 0: top,    left → right */
    "Zone 3", "Zone 2", "Zone 1",   /* row 1: middle, left → right */
    "Zone 1", "Zone 2", "Zone 3",   /* row 2: bottom, left → right */
};

/*
 * Left-strip level labels.  Each ordinal ("3rd", "2nd", "1st") fits on one
 * horizontal line, with "Level" on the line below, stacked vertically.
 * Row order top → bottom: 3rd Level, 2nd Level, 1st Level.
 */
static const char *s_level_vtext[GRID_ROWS] = {
    "3rd\nLevel",   /* 3rd Level */
    "2nd\nLevel",   /* 2nd Level */
    "1st\nLevel",   /* 1st Level */
};

/* ── per-cell state ─────────────────────────────────────────────────── */
typedef struct {
    lv_obj_t *slider;        /* horizontal fill-bar slider */
    lv_obj_t *count_lbl;     /* numeric count label below the slider */
    int32_t   value;
    lv_obj_t *shimmer_clip;  /* clipping container over the fill region */
    lv_obj_t *shimmer_stripe;/* the sweeping stripe inside the clip */
    int32_t   shimmer_x;     /* current stripe x in clip-relative pixels */
    bool      shimmer_rtl;   /* true for RTL (middle row) bars */
    int64_t   last_update_us;/* esp_timer_get_time() at last MQTT update, 0 = never */
    bool      data_received; /* true once at least one MQTT update has arrived */
    bool      stale;         /* true when no update for ≥ STALE_THRESHOLD_US */
} bar_cell_t;

static bar_cell_t  s_cells[BAR_COUNT];
static lv_timer_t *s_flash_timer   = NULL;
static bool        s_flash_state   = false;
static lv_timer_t *s_stale_timer   = NULL;
static lv_timer_t *s_shimmer_timer = NULL;
static lv_color_t  s_row_bg_colors[GRID_ROWS]; /* row background colors */
static lv_obj_t   *s_bg_bands[GRID_ROWS];      /* full-width background bands */

/* ── status bar widgets ─────────────────────────────────────────────── */
static lv_obj_t   *s_status_wifi_lbl = NULL;  /* WiFi symbol + state        */
static lv_obj_t   *s_status_ip_lbl   = NULL;  /* IP address string          */
static lv_obj_t   *s_status_mqtt_lbl = NULL;  /* MQTT connected indicator   */
static lv_obj_t   *s_status_rx_lbl   = NULL;  /* MQTT RX activity indicator */
static lv_timer_t *s_rx_dim_timer    = NULL;  /* dims RX indicator after 2 s */
static lv_obj_t   *s_status_upd_lbl  = NULL;  /* successful UI update count */

/* Counts how many times ui_update_zone_count() successfully held the LVGL
 * lock and wrote slider+label values.  Visible on the status bar as "UPD:N".
 * Wraps naturally at UINT32_MAX. */
static uint32_t    s_upd_count       = 0;

/* ── colour helpers ─────────────────────────────────────────────────── */
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
        for (int r = 0; r < GRID_ROWS; r++)
            lv_obj_set_style_bg_color(s_bg_bands[r], s_row_bg_colors[r], 0);
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

/* ── stale-data check timer (1 Hz) ──────────────────────────────────── */

/*
 * Runs every second inside the LVGL task.
 * For each cell that has received at least one MQTT update, checks whether
 * the last update was more than STALE_THRESHOLD_US ago.  If so, the count
 * label is changed to "---" in grey to indicate stale data.
 */
static void stale_check_cb(lv_timer_t *t)
{
    (void)t;
    int64_t now_us = esp_timer_get_time();
    for (int i = 0; i < BAR_COUNT; i++) {
        if (!s_cells[i].data_received) continue;

        bool is_stale = (now_us - s_cells[i].last_update_us) >= STALE_THRESHOLD_US;
        if (is_stale == s_cells[i].stale) continue;   /* no change */

        s_cells[i].stale = is_stale;
        if (is_stale) {
            lv_label_set_text(s_cells[i].count_lbl, "---");
            lv_obj_set_style_text_color(s_cells[i].count_lbl,
                                        lv_color_make(150, 150, 150), 0);
        }
        /* Non-stale restore is handled by ui_update_zone_count() */
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

/* ── RX dim callback ─────────────────────────────────────────────────── */

/*
 * Fires 2 s after the last MQTT message was received; dims the RX dot.
 * The timer is created without a fixed repeat count and is explicitly
 * deleted here so there is no ambiguity about when LVGL releases it.
 */
static void rx_dim_cb(lv_timer_t *t)
{
    lv_timer_del(t);     /* explicit one-shot: delete before touching UI */
    s_rx_dim_timer = NULL;
    if (s_status_rx_lbl) {
        lv_label_set_text(s_status_rx_lbl, "RX: --");
        lv_obj_set_style_text_color(s_status_rx_lbl,
                                    lv_color_make(130, 130, 130), 0);
    }
}

/* ── public API ──────────────────────────────────────────────────────── */
void app_ui_init(void)
{
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_set_style_border_width(scr, 0, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(scr, lv_color_make(20, 45, 100), 0);

    /* Row background colours:
     * 1st and 3rd Level share the same dark navy; 2nd Level is a lighter blue.
     * Stored globally so the flash timer can restore them after a red flash.
     * Row order (top → bottom): row 0 = 3rd Level, row 1 = 2nd Level, row 2 = 1st Level. */
    s_row_bg_colors[0] = lv_color_make(20,  45, 100);   /* 3rd Level – dark navy (same as 1st) */
    s_row_bg_colors[1] = lv_color_make(50, 120, 190);   /* 2nd Level – lighter steel blue      */
    s_row_bg_colors[2] = lv_color_make(20,  45, 100);   /* 1st Level – dark navy (same as 3rd) */

    lv_color_t row_border_colors[GRID_ROWS];
    row_border_colors[0] = lv_color_make(50,  75, 135);
    row_border_colors[1] = lv_color_make(80, 155, 215);
    row_border_colors[2] = lv_color_make(50,  75, 135);

    /* ── Full-width background bands (created before left_strip/grid so they sit behind) ── */
    lv_coord_t band_h = LVGL_PORT_V_RES / GRID_ROWS;
    for (int r = 0; r < GRID_ROWS; r++) {
        /* Last band gets any remaining pixels to avoid a gap at the bottom */
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
        lv_obj_set_style_bg_opa(seg, LV_OPA_TRANSP, 0);   /* background band shows through */
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

    for (int i = 0; i < BAR_COUNT; i++) {
        int col = i % GRID_COLS;
        int row = i / GRID_COLS;

        s_cells[i].value         = 0;
        s_cells[i].last_update_us = 0;
        s_cells[i].data_received  = false;
        s_cells[i].stale          = false;

        /* ── Cell container ───────────────────────────────────────── */
        lv_obj_t *cell = lv_obj_create(grid);
        lv_obj_set_grid_cell(cell,
                             LV_GRID_ALIGN_STRETCH, col, 1,
                             LV_GRID_ALIGN_STRETCH, row, 1);
        lv_obj_set_style_pad_all(cell, 4, 0);
        lv_obj_set_style_bg_opa(cell, LV_OPA_TRANSP, 0);   /* background band shows through */
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
        lv_obj_add_event_cb(slider, slider_changed_cb, LV_EVENT_VALUE_CHANGED,
                            (void *)(intptr_t)i);
        s_cells[i].slider = slider;

        /* Shimmer stripe – "charging battery" wave sweeping left-to-right */
        add_shimmer(slider, i, false);

        /* ── Count label – shows numeric MQTT count below the bar ─── */
        lv_obj_t *count_lbl = lv_label_create(cell);
        lv_label_set_text(count_lbl, "0");
        lv_obj_set_style_text_color(count_lbl, lv_color_white(), 0);
        lv_obj_set_style_text_font(count_lbl, &lv_font_montserrat_24, 0);
        lv_obj_set_style_text_align(count_lbl, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(count_lbl, lv_pct(100));
        s_cells[i].count_lbl = count_lbl;
    }

    /* ── Status bar: 26 px strip at the very bottom of the screen ──── */
    lv_obj_t *status_bar = lv_obj_create(scr);
    lv_obj_add_flag(status_bar, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_clear_flag(status_bar,
                      LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(status_bar, 0, LVGL_PORT_V_RES - STATUS_BAR_H);
    lv_obj_set_size(status_bar, LVGL_PORT_H_RES, STATUS_BAR_H);
    lv_obj_set_style_bg_color(status_bar, lv_color_make(10, 20, 40), 0);
    lv_obj_set_style_bg_opa(status_bar, LV_OPA_90, 0);
    lv_obj_set_style_border_color(status_bar, lv_color_make(60, 90, 150), 0);
    lv_obj_set_style_border_width(status_bar, 1, 0);
    lv_obj_set_style_border_side(status_bar, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_radius(status_bar, 0, 0);
    lv_obj_set_style_pad_all(status_bar, 3, 0);

    /* WiFi symbol + connection state (left-aligned) */
    lv_obj_t *wifi_lbl = lv_label_create(status_bar);
    lv_label_set_text(wifi_lbl, LV_SYMBOL_WIFI "  No WiFi");
    lv_obj_set_style_text_color(wifi_lbl, lv_color_make(130, 130, 130), 0);
    lv_obj_set_style_text_font(wifi_lbl, &lv_font_montserrat_12, 0);
    lv_obj_align(wifi_lbl, LV_ALIGN_LEFT_MID, 4, 0);
    s_status_wifi_lbl = wifi_lbl;

    /* IP address label (immediately right of the WiFi label) */
    lv_obj_t *ip_lbl = lv_label_create(status_bar);
    lv_label_set_text(ip_lbl, "");
    lv_obj_set_style_text_color(ip_lbl, lv_color_make(200, 200, 200), 0);
    lv_obj_set_style_text_font(ip_lbl, &lv_font_montserrat_12, 0);
    lv_obj_align_to(ip_lbl, wifi_lbl, LV_ALIGN_OUT_RIGHT_MID, 6, 0);
    s_status_ip_lbl = ip_lbl;

    /* MQTT RX indicator (right-aligned) */
    lv_obj_t *rx_lbl = lv_label_create(status_bar);
    lv_label_set_text(rx_lbl, "RX: --");
    lv_obj_set_style_text_color(rx_lbl, lv_color_make(130, 130, 130), 0);
    lv_obj_set_style_text_font(rx_lbl, &lv_font_montserrat_12, 0);
    lv_obj_align(rx_lbl, LV_ALIGN_RIGHT_MID, -4, 0);
    s_status_rx_lbl = rx_lbl;

    /* MQTT connection indicator (centre of the status bar) */
    lv_obj_t *mqtt_lbl = lv_label_create(status_bar);
    lv_label_set_text(mqtt_lbl, "MQTT: --");
    lv_obj_set_style_text_color(mqtt_lbl, lv_color_make(130, 130, 130), 0);
    lv_obj_set_style_text_font(mqtt_lbl, &lv_font_montserrat_12, 0);
    lv_obj_align(mqtt_lbl, LV_ALIGN_CENTER, 0, 0);
    s_status_mqtt_lbl = mqtt_lbl;

    /* Diagnostic update counter (between MQTT centre and RX right).
     * Shows how many times ui_update_zone_count() successfully wrote
     * slider/label values through the LVGL lock.  If MQTT is "OK" but
     * this stays at 0 the lock is timing out. */
    lv_obj_t *upd_lbl = lv_label_create(status_bar);
    lv_label_set_text(upd_lbl, "UPD:0");
    lv_obj_set_style_text_color(upd_lbl, lv_color_make(130, 130, 130), 0);
    lv_obj_set_style_text_font(upd_lbl, &lv_font_montserrat_12, 0);
    lv_obj_align(upd_lbl, LV_ALIGN_CENTER, 80, 0);
    s_status_upd_lbl = upd_lbl;

    /* ── Stale-check timer: fires every 1 second ───────────────────── */
    s_stale_timer = lv_timer_create(stale_check_cb, 1000, NULL);

    /* ── Shimmer timer: advances the wave stripes at ~25 fps ────── */
    s_shimmer_timer = lv_timer_create(shimmer_timer_cb, SHIMMER_TICK_MS, NULL);
}

/* ── MQTT-driven zone count update ──────────────────────────────────── */

/*
 * Map (level, zone) to flat cell index in the 3×3 grid.
 *
 * Grid layout (top → bottom):
 *   row 0 = 3rd Level : Zone 1, Zone 2, Zone 3  (col 0, 1, 2)
 *   row 1 = 2nd Level : Zone 3, Zone 2, Zone 1  (col 0, 1, 2 – reversed)
 *   row 2 = 1st Level : Zone 1, Zone 2, Zone 3  (col 0, 1, 2)
 *
 * Returns -1 for out-of-range inputs.
 */
static int zone_to_cell(int level, int zone)
{
    if (level < 1 || level > 3) return -1;
    if (zone  < 1 || zone  > 3) return -1;

    int row = 3 - level;   /* level 1 → row 2, level 2 → row 1, level 3 → row 0 */
    int col;
    if (level == 2) {
        col = 3 - zone;    /* 2nd Level is reversed: zone 1→col2, zone 3→col0 */
    } else {
        col = zone - 1;    /* 1st / 3rd Level: zone 1→col0, zone 3→col2 */
    }
    return row * GRID_COLS + col;
}

/*
 * Update the fill-bar and count label for the given level/zone with a new
 * MQTT count value.  Safe to call from any FreeRTOS task; acquires the LVGL
 * mutex internally.
 */
void ui_update_zone_count(int level, int zone, int count)
{
    int idx = zone_to_cell(level, zone);
    if (idx < 0) {
        ESP_LOGW(TAG_UI, "zone_to_cell returned -1 for L%d Z%d – ignoring", level, zone);
        return;
    }

    /* Clamp value for the bar (0–VALUE_MAX); label shows the raw count */
    int32_t bar_val = count;
    if (bar_val < 0)         bar_val = 0;
    if (bar_val > VALUE_MAX) bar_val = VALUE_MAX;

    ESP_LOGD(TAG_UI, "update L%d Z%d cnt=%d → cell[%d] bar=%"PRId32,
             level, zone, count, idx, bar_val);

    /* Record update timestamp before taking the LVGL lock so the stale
     * timer never races with a fresh write.                              */
    s_cells[idx].last_update_us = esp_timer_get_time();
    s_cells[idx].data_received  = true;

    if (lvgl_port_lock(LVGL_LOCK_TIMEOUT_MS)) {
        s_cells[idx].value = bar_val;
        s_cells[idx].stale = false;

        lv_slider_set_value(s_cells[idx].slider, bar_val, LV_ANIM_OFF);
        update_cell(idx);

        /* Update count label with the raw (unclamped) value */
        char buf[16];
        snprintf(buf, sizeof(buf), "%d", count);
        lv_label_set_text(s_cells[idx].count_lbl, buf);
        lv_obj_set_style_text_color(s_cells[idx].count_lbl, lv_color_white(), 0);

        /* Advance on-screen diagnostic counter */
        s_upd_count++;
        if (s_status_upd_lbl) {
            char ubuf[16];
            snprintf(ubuf, sizeof(ubuf), "UPD:%"PRIu32, s_upd_count);
            lv_label_set_text(s_status_upd_lbl, ubuf);
            lv_obj_set_style_text_color(s_status_upd_lbl,
                                        lv_color_make(0, 200, 80), 0);
        }

        lvgl_port_unlock();
    } else {
        ESP_LOGW(TAG_UI, "LVGL lock timeout – update dropped (L%d Z%d cnt=%d cell[%d])",
                 level, zone, count, idx);
    }
}

/* ── Status bar public API ───────────────────────────────────────────── */

/*
 * Update the WiFi symbol and IP address in the status bar.
 * Call from wifi_manager on IP_EVENT_STA_GOT_IP (connected=true) and
 * WIFI_EVENT_STA_DISCONNECTED (connected=false).
 */
void ui_set_wifi_status(bool connected, const char *ip_str)
{
    if (!s_status_wifi_lbl) return;

    if (lvgl_port_lock(LVGL_LOCK_TIMEOUT_MS)) {
        if (connected) {
            lv_label_set_text(s_status_wifi_lbl, LV_SYMBOL_WIFI "  Connected");
            lv_obj_set_style_text_color(s_status_wifi_lbl,
                                        lv_color_make(0, 200, 80), 0);
            lv_label_set_text(s_status_ip_lbl, ip_str ? ip_str : "");
        } else {
            lv_label_set_text(s_status_wifi_lbl, LV_SYMBOL_WIFI "  No WiFi");
            lv_obj_set_style_text_color(s_status_wifi_lbl,
                                        lv_color_make(130, 130, 130), 0);
            lv_label_set_text(s_status_ip_lbl, "");
        }
        /* Re-align IP label after the WiFi label text may have changed width */
        lv_obj_align_to(s_status_ip_lbl, s_status_wifi_lbl,
                        LV_ALIGN_OUT_RIGHT_MID, 6, 0);
        lvgl_port_unlock();
    }
}

/*
 * Update the MQTT broker connection indicator.
 * Call from mqtt_handler on MQTT_EVENT_CONNECTED (connected=true) and
 * MQTT_EVENT_DISCONNECTED (connected=false).
 */
void ui_set_mqtt_status(bool connected)
{
    if (!s_status_mqtt_lbl) return;

    if (lvgl_port_lock(LVGL_LOCK_TIMEOUT_MS)) {
        if (connected) {
            lv_label_set_text(s_status_mqtt_lbl, "MQTT: OK");
            lv_obj_set_style_text_color(s_status_mqtt_lbl,
                                        lv_color_make(0, 200, 80), 0);
        } else {
            lv_label_set_text(s_status_mqtt_lbl, "MQTT: --");
            lv_obj_set_style_text_color(s_status_mqtt_lbl,
                                        lv_color_make(130, 130, 130), 0);
        }
        lvgl_port_unlock();
    }
}

/*
 * Signal that an MQTT message was just received from the publisher.
 * Lights the RX indicator green; an internal timer dims it after 2 seconds.
 */
void ui_notify_mqtt_rx(void)
{
    if (!s_status_rx_lbl) return;

    if (lvgl_port_lock(LVGL_LOCK_TIMEOUT_MS)) {
        lv_label_set_text(s_status_rx_lbl, "RX: " LV_SYMBOL_OK);
        lv_obj_set_style_text_color(s_status_rx_lbl,
                                    lv_color_make(0, 200, 80), 0);

        /* Restart (or start) the 2 s dim timer.
         * Both paths run under the LVGL lock, so s_rx_dim_timer is always
         * consistent with whether the timer object is alive. */
        if (s_rx_dim_timer) {
            lv_timer_reset(s_rx_dim_timer);
        } else {
            s_rx_dim_timer = lv_timer_create(rx_dim_cb, 2000, NULL);
            /* No repeat_count set – the callback deletes the timer itself */
        }

        lvgl_port_unlock();
    }
}
