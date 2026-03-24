/*
 * ui.c – 3×3 grid of fill-bar sliders driven by MQTT zone counts
 *
 * Layout (800×480):
 *   ┌─────────────────────────────────────────────┐  ← row 0: 30 px top header
 *   │  North Pick Mod                     [WiFi]  │
 *   ├──────┬─────────────────────────────────────-┤  ← content starts at y=30
 *   │      │  Zone 1 │  Zone 2 │  Zone 3          │  3rd Level
 *   │labels│─────────┼─────────┼──────────────────┤
 *   │      │  Zone 3 │  Zone 2 │  Zone 1          │  2nd Level
 *   │      │─────────┼─────────┼──────────────────┤
 *   │      │  Zone 1 │  Zone 2 │  Zone 3          │  1st Level
 *   └──────┴─────────┴─────────┴──────────────────┘
 *
 * Each cell shows:
 *   • A zone label at the top.
 *   • A horizontal fill-bar slider (shimmer "charging battery" wave).
 *   • A numeric count label (raw MQTT value; shows "---" after 10 s stale).
 *
 * Top header bar:
 *   • Left:  title "North Pick Mod"
 *   • Right: WiFi symbol (tap to open connection-info popup)
 *              green  – WiFi + MQTT connected
 *              red    – WiFi connected, MQTT not connected
 *              grey   – not connected
 *
 * Popup (on WiFi tap): IP address, MQTT status, UPD count, RX count.
 */

#include "ui.h"
#include "lvgl.h"
#include "lvgl_port.h"
#include "esp_timer.h"
#include "esp_log.h"
#include <inttypes.h>
#include <stdatomic.h>
#include <string.h>

#define TAG_UI "ui"

/* ── layout constants ───────────────────────────────────────────────── */
#define BAR_COUNT   9
#define GRID_COLS   3
#define GRID_ROWS   3
#define VALUE_MAX   40

#define HEADER_H        30                          /* top header height    */
#define CONTENT_Y       HEADER_H                    /* content area top     */
#define CONTENT_H       (LVGL_PORT_V_RES - HEADER_H)/* content area height  */

/* Staleness threshold: 10 seconds in microseconds */
#define STALE_THRESHOLD_US  (10LL * 1000000LL)

/* Left-strip width for row level labels */
#define LEFT_STRIP_W    60

/* Width of the WiFi button hit-target in the header */
#define WIFI_BTN_W      56

/* Slider bar */
#define SLIDER_H            60
#define SLIDER_W_SWEEP     260    /* estimated max slider width (≥ actual) */

/* Shimmer ("charging battery" wave) */
#define SHIMMER_W           36
#define SHIMMER_TICK_MS     40    /* ~25 fps                               */
#define SHIMMER_PX_PER_TICK  5   /* ~125 px/s                             */

/* Max wait for the LVGL mutex from an external task */
#define LVGL_LOCK_TIMEOUT_MS  10

/* ── zone / level labels ────────────────────────────────────────────── */
static const char *s_zone_names[BAR_COUNT] = {
    "Zone 1", "Zone 2", "Zone 3",   /* row 0: 3rd Level, left → right */
    "Zone 3", "Zone 2", "Zone 1",   /* row 1: 2nd Level, reversed     */
    "Zone 1", "Zone 2", "Zone 3",   /* row 2: 1st Level, left → right */
};

static const char *s_level_vtext[GRID_ROWS] = {
    "3rd\nLevel",
    "2nd\nLevel",
    "1st\nLevel",
};

/* ── per-cell state ─────────────────────────────────────────────────── */
typedef struct {
    lv_obj_t *slider;
    lv_obj_t *count_lbl;
    int32_t   value;
    lv_obj_t *shimmer_clip;
    lv_obj_t *shimmer_stripe;
    int32_t   shimmer_x;
    bool      shimmer_rtl;
    int64_t   last_update_us;
    bool      data_received;
    bool      stale;
} bar_cell_t;

static bar_cell_t  s_cells[BAR_COUNT];
static lv_timer_t *s_flash_timer    = NULL;
static bool        s_flash_state    = false;
static lv_timer_t *s_stale_timer    = NULL;
static lv_timer_t *s_shimmer_timer  = NULL;
static lv_color_t  s_row_bg_colors[GRID_ROWS];
static lv_obj_t   *s_bg_bands[GRID_ROWS];

/* ── connection state (cached for popup) ────────────────────────────── */
static bool     s_wifi_connected   = false;
static bool     s_mqtt_connected   = false;
static char     s_ip_str[32]       = "---";
static uint32_t s_upd_count        = 0;
static atomic_uint s_rx_count      = 0;

/* ── header / popup widgets ─────────────────────────────────────────── */
static lv_obj_t *s_header_wifi_icon = NULL;
static lv_obj_t *s_popup            = NULL;

/* ── colour helpers ─────────────────────────────────────────────────── */

static lv_color_t value_to_color(int32_t v)
{
    uint8_t r = 0, g = 0, b = 0;
    if (v <= 20) {
        r = 0;   g = 200;
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

/* WiFi icon colour: green=both connected, red=wifi only, grey=none */
static lv_color_t wifi_icon_color(void)
{
    if (s_wifi_connected && s_mqtt_connected) return lv_color_make(0, 200, 80);
    if (s_wifi_connected)                     return lv_color_make(220, 0, 0);
    return lv_color_make(130, 130, 130);
}

/* Must be called while the LVGL lock is held. */
static void update_wifi_icon(void)
{
    if (s_header_wifi_icon) {
        lv_obj_set_style_text_color(s_header_wifi_icon, wifi_icon_color(), 0);
    }
}

/* ── flash timer (background flashes red when any bar is at max) ────── */

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

/* ── slider event callback ──────────────────────────────────────────── */

static void slider_changed_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    int32_t new_val = lv_slider_get_value(s_cells[idx].slider);
    if (new_val == s_cells[idx].value) return;
    s_cells[idx].value = new_val;
    update_cell(idx);
}

/* ── stale-data check timer (1 Hz) ──────────────────────────────────── */

static void stale_check_cb(lv_timer_t *t)
{
    (void)t;
    int64_t now_us = esp_timer_get_time();
    for (int i = 0; i < BAR_COUNT; i++) {
        if (!s_cells[i].data_received) continue;
        bool is_stale = (now_us - s_cells[i].last_update_us) >= STALE_THRESHOLD_US;
        if (is_stale == s_cells[i].stale) continue;
        s_cells[i].stale = is_stale;
        if (is_stale) {
            lv_label_set_text(s_cells[i].count_lbl, "---");
            lv_obj_set_style_text_color(s_cells[i].count_lbl,
                                        lv_color_make(150, 150, 150), 0);
        }
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

/* ── info popup ──────────────────────────────────────────────────────── */

static void popup_close_cb(lv_event_t *e)
{
    (void)e;
    if (s_popup) {
        lv_obj_del(s_popup);
        s_popup = NULL;
    }
}

static void wifi_icon_click_cb(lv_event_t *e)
{
    (void)e;

    /* Toggle: tap again to close */
    if (s_popup) {
        lv_obj_del(s_popup);
        s_popup = NULL;
        return;
    }

    /* Full-screen semi-transparent overlay (click anywhere to close) */
    s_popup = lv_obj_create(lv_scr_act());
    lv_obj_add_flag(s_popup, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_set_size(s_popup, LVGL_PORT_H_RES, LVGL_PORT_V_RES);
    lv_obj_set_pos(s_popup, 0, 0);
    lv_obj_set_style_bg_color(s_popup, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_popup, LV_OPA_50, 0);
    lv_obj_set_style_border_width(s_popup, 0, 0);
    lv_obj_set_style_radius(s_popup, 0, 0);
    lv_obj_clear_flag(s_popup, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_popup, popup_close_cb, LV_EVENT_CLICKED, NULL);

    /* Info box */
    lv_obj_t *box = lv_obj_create(s_popup);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(box, 340, 230);
    lv_obj_center(box);
    lv_obj_set_style_bg_color(box, lv_color_make(20, 40, 80), 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(box, lv_color_make(80, 120, 200), 0);
    lv_obj_set_style_border_width(box, 2, 0);
    lv_obj_set_style_radius(box, 8, 0);
    lv_obj_set_style_pad_all(box, 14, 0);
    lv_obj_set_style_pad_row(box, 8, 0);
    lv_obj_set_layout(box, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);

    /* Title */
    lv_obj_t *title = lv_label_create(box);
    lv_label_set_text(title, "Connection Info");
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);

    char buf[64];

    /* IP address */
    lv_obj_t *ip_lbl = lv_label_create(box);
    snprintf(buf, sizeof(buf), "IP:   %s", s_ip_str);
    lv_label_set_text(ip_lbl, buf);
    lv_obj_set_style_text_color(ip_lbl, lv_color_make(200, 220, 255), 0);
    lv_obj_set_style_text_font(ip_lbl, &lv_font_montserrat_16, 0);

    /* MQTT status */
    lv_obj_t *mqtt_lbl = lv_label_create(box);
    snprintf(buf, sizeof(buf), "MQTT: %s",
             s_mqtt_connected ? "Connected" : "Disconnected");
    lv_label_set_text(mqtt_lbl, buf);
    lv_color_t mc = s_mqtt_connected
        ? lv_color_make(0, 200, 80)
        : lv_color_make(220, 80, 80);
    lv_obj_set_style_text_color(mqtt_lbl, mc, 0);
    lv_obj_set_style_text_font(mqtt_lbl, &lv_font_montserrat_16, 0);

    /* UPD count */
    lv_obj_t *upd_lbl = lv_label_create(box);
    snprintf(buf, sizeof(buf), "UPD: %"PRIu32, s_upd_count);
    lv_label_set_text(upd_lbl, buf);
    lv_obj_set_style_text_color(upd_lbl, lv_color_make(200, 220, 255), 0);
    lv_obj_set_style_text_font(upd_lbl, &lv_font_montserrat_16, 0);

    /* RX count */
    lv_obj_t *rx_lbl = lv_label_create(box);
    snprintf(buf, sizeof(buf), "RX:  %"PRIu32, (uint32_t)atomic_load(&s_rx_count));
    lv_label_set_text(rx_lbl, buf);
    lv_obj_set_style_text_color(rx_lbl, lv_color_make(200, 220, 255), 0);
    lv_obj_set_style_text_font(rx_lbl, &lv_font_montserrat_16, 0);

    /* Close hint */
    lv_obj_t *hint = lv_label_create(box);
    lv_label_set_text(hint, "(tap anywhere to close)");
    lv_obj_set_style_text_color(hint, lv_color_make(150, 150, 150), 0);
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_12, 0);
}

/* ── public API ──────────────────────────────────────────────────────── */

void app_ui_init(void)
{
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_set_style_border_width(scr, 0, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(scr, lv_color_make(20, 45, 100), 0);

    /* Row background colours (top→bottom: 3rd / 2nd / 1st Level).
     * Stored globally so flash_cb can restore them after a red flash. */
    s_row_bg_colors[0] = lv_color_make(20,  45, 100);   /* 3rd Level – dark navy      */
    s_row_bg_colors[1] = lv_color_make(50, 120, 190);   /* 2nd Level – lighter blue   */
    s_row_bg_colors[2] = lv_color_make(20,  45, 100);   /* 1st Level – dark navy      */

    lv_color_t row_border_colors[GRID_ROWS];
    row_border_colors[0] = lv_color_make(50,  75, 135);
    row_border_colors[1] = lv_color_make(80, 155, 215);
    row_border_colors[2] = lv_color_make(50,  75, 135);

    /* ── Full-width background bands (below header) ─────────────────── */
    lv_coord_t band_h = CONTENT_H / GRID_ROWS;
    for (int r = 0; r < GRID_ROWS; r++) {
        lv_coord_t this_h = (r == GRID_ROWS - 1)
                            ? (CONTENT_H - r * band_h)
                            : band_h;
        lv_obj_t *band = lv_obj_create(scr);
        lv_obj_add_flag(band, LV_OBJ_FLAG_IGNORE_LAYOUT);
        lv_obj_clear_flag(band, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_pos(band, 0, CONTENT_Y + r * band_h);
        lv_obj_set_size(band, LVGL_PORT_H_RES, this_h);
        lv_obj_set_style_bg_color(band, s_row_bg_colors[r], 0);
        lv_obj_set_style_bg_opa(band, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(band, 0, 0);
        lv_obj_set_style_radius(band, 0, 0);
        lv_obj_set_style_pad_all(band, 0, 0);
        s_bg_bands[r] = band;
    }

    /* ── Left strip: level labels ────────────────────────────────────── */
    lv_obj_t *left_strip = lv_obj_create(scr);
    lv_obj_set_size(left_strip, LEFT_STRIP_W, CONTENT_H);
    lv_obj_set_pos(left_strip, 0, CONTENT_Y);
    lv_obj_set_style_bg_opa(left_strip, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(left_strip, 0, 0);
    lv_obj_set_style_pad_all(left_strip, 0, 0);
    lv_obj_clear_flag(left_strip, LV_OBJ_FLAG_SCROLLABLE);

    lv_coord_t seg_h = CONTENT_H / GRID_ROWS;
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

    /* ── 3×3 grid container ──────────────────────────────────────────── */
    static lv_coord_t col_dsc[] = {
        LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST
    };
    static lv_coord_t row_dsc[] = {
        LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST
    };

    lv_obj_t *grid = lv_obj_create(scr);
    lv_obj_set_size(grid, LVGL_PORT_H_RES - LEFT_STRIP_W, CONTENT_H);
    lv_obj_set_pos(grid, LEFT_STRIP_W, CONTENT_Y);
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

        s_cells[i].value          = 0;
        s_cells[i].last_update_us = 0;
        s_cells[i].data_received  = false;
        s_cells[i].stale          = false;

        /* Cell container */
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

        /* Zone label */
        lv_obj_t *zone_lbl = lv_label_create(cell);
        lv_label_set_text(zone_lbl, s_zone_names[i]);
        lv_obj_set_style_text_color(zone_lbl, lv_color_white(), 0);
        lv_obj_set_style_text_align(zone_lbl, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(zone_lbl, lv_pct(100));

        /* Horizontal fill-bar slider */
        lv_obj_t *slider = lv_slider_create(cell);
        lv_obj_set_size(slider, lv_pct(100), SLIDER_H);
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
        lv_obj_add_event_cb(slider, slider_changed_cb, LV_EVENT_VALUE_CHANGED,
                            (void *)(intptr_t)i);
        s_cells[i].slider = slider;

        add_shimmer(slider, i, false);

        /* Count label */
        lv_obj_t *count_lbl = lv_label_create(cell);
        lv_label_set_text(count_lbl, "0");
        lv_obj_set_style_text_color(count_lbl, lv_color_white(), 0);
        lv_obj_set_style_text_font(count_lbl, &lv_font_montserrat_24, 0);
        lv_obj_set_style_text_align(count_lbl, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(count_lbl, lv_pct(100));
        s_cells[i].count_lbl = count_lbl;
    }

    /* ── Top header bar ─────────────────────────────────────────────── */
    lv_obj_t *header = lv_obj_create(scr);
    lv_obj_add_flag(header, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(header, 0, 0);
    lv_obj_set_size(header, LVGL_PORT_H_RES, HEADER_H);
    lv_obj_set_style_bg_color(header, lv_color_make(10, 20, 50), 0);
    lv_obj_set_style_bg_opa(header, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(header, lv_color_make(60, 90, 150), 0);
    lv_obj_set_style_border_width(header, 1, 0);
    lv_obj_set_style_border_side(header, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_radius(header, 0, 0);
    lv_obj_set_style_pad_all(header, 4, 0);

    /* Title */
    lv_obj_t *title_lbl = lv_label_create(header);
    lv_label_set_text(title_lbl, "North Pick Mod");
    lv_obj_set_style_text_color(title_lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(title_lbl, &lv_font_montserrat_16, 0);
    lv_obj_align(title_lbl, LV_ALIGN_LEFT_MID, 4, 0);

    /* WiFi button area (larger hit target makes touchscreen easier to use) */
    lv_obj_t *wifi_btn = lv_obj_create(header);
    lv_obj_add_flag(wifi_btn, LV_OBJ_FLAG_IGNORE_LAYOUT | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(wifi_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(wifi_btn, WIFI_BTN_W, HEADER_H - 4);
    lv_obj_align(wifi_btn, LV_ALIGN_RIGHT_MID, -2, 0);
    lv_obj_set_style_bg_opa(wifi_btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(wifi_btn, 0, 0);
    lv_obj_set_style_radius(wifi_btn, 4, 0);
    lv_obj_set_style_pad_all(wifi_btn, 0, 0);
    lv_obj_add_event_cb(wifi_btn, wifi_icon_click_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *wifi_icon = lv_label_create(wifi_btn);
    lv_label_set_text(wifi_icon, LV_SYMBOL_WIFI);
    lv_obj_set_style_text_color(wifi_icon, wifi_icon_color(), 0);
    lv_obj_set_style_text_font(wifi_icon, &lv_font_montserrat_20, 0);
    lv_obj_center(wifi_icon);
    s_header_wifi_icon = wifi_icon;

    /* ── Timers ─────────────────────────────────────────────────────── */
    s_stale_timer   = lv_timer_create(stale_check_cb,   1000,          NULL);
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

    int row = 3 - level;   /* level 1→row 2, level 2→row 1, level 3→row 0 */
    int col = (level == 2) ? (3 - zone) : (zone - 1);
    return row * GRID_COLS + col;
}

void ui_update_zone_count(int level, int zone, int count)
{
    int idx = zone_to_cell(level, zone);
    if (idx < 0) {
        ESP_LOGW(TAG_UI, "zone_to_cell returned -1 for L%d Z%d – ignoring", level, zone);
        return;
    }

    int32_t bar_val = count;
    if (bar_val < 0)         bar_val = 0;
    if (bar_val > VALUE_MAX) bar_val = VALUE_MAX;

    ESP_LOGD(TAG_UI, "update L%d Z%d cnt=%d → cell[%d] bar=%"PRId32,
             level, zone, count, idx, bar_val);

    s_cells[idx].last_update_us = esp_timer_get_time();
    s_cells[idx].data_received  = true;

    if (lvgl_port_lock(LVGL_LOCK_TIMEOUT_MS)) {
        s_cells[idx].value = bar_val;
        s_cells[idx].stale = false;

        lv_slider_set_value(s_cells[idx].slider, bar_val, LV_ANIM_OFF);
        update_cell(idx);

        char buf[16];
        snprintf(buf, sizeof(buf), "%d", count);
        lv_label_set_text(s_cells[idx].count_lbl, buf);
        lv_obj_set_style_text_color(s_cells[idx].count_lbl, lv_color_white(), 0);

        s_upd_count++;
        lvgl_port_unlock();
    } else {
        ESP_LOGW(TAG_UI, "LVGL lock timeout – update dropped (L%d Z%d cnt=%d cell[%d])",
                 level, zone, count, idx);
    }
}

/* ── Connection status API ───────────────────────────────────────────── */

void ui_set_wifi_status(bool connected, const char *ip_str)
{
    if (lvgl_port_lock(LVGL_LOCK_TIMEOUT_MS)) {
        s_wifi_connected = connected;
        if (connected && ip_str) {
            snprintf(s_ip_str, sizeof(s_ip_str), "%s", ip_str);
        } else if (!connected) {
            snprintf(s_ip_str, sizeof(s_ip_str), "%s", "---");
        }
        update_wifi_icon();
        lvgl_port_unlock();
    }
}

void ui_set_mqtt_status(bool connected)
{
    if (lvgl_port_lock(LVGL_LOCK_TIMEOUT_MS)) {
        s_mqtt_connected = connected;
        update_wifi_icon();
        lvgl_port_unlock();
    }
}

void ui_notify_mqtt_rx(void)
{
    /* Increment diagnostic RX counter; shown in the popup when opened.
     * Called from the MQTT task while the popup reads from the LVGL task –
     * use atomic fetch-add to avoid a data race. */
    atomic_fetch_add(&s_rx_count, 1u);
}
