/*
 * ui.c – Background color-control UI
 *
 * Displays:
 *   • A full-screen color swatch whose color is determined by:
 *       – Hue slider       (0–359): drag to pick a color
 *       – Brightness entry (0–100): tap +/- buttons to change lightness
 *   • A semi-transparent control panel pinned to the bottom of the screen.
 *
 * Color model: HSV with saturation fixed at 100 %, so the slider
 * sweeps the full rainbow and the number entry dims / brightens it.
 */

#include "ui.h"
#include "lvgl.h"

/* ── internal state ─────────────────────────────────────────────────── */
static lv_obj_t *spinbox;
static int32_t   hue_val    = 180;  /* 0–359 */
static int32_t   bright_val = 70;   /* 0–100 */

/* ── helpers ─────────────────────────────────────────────────────────── */
static void refresh_bg(void)
{
    lv_color_t c = lv_color_hsv_to_rgb((uint16_t)hue_val, 100,
                                        (uint8_t)bright_val);
    lv_obj_set_style_bg_color(lv_scr_act(), c, 0);
}

/* ── event callbacks ─────────────────────────────────────────────────── */
static void slider_cb(lv_event_t *e)
{
    hue_val = lv_slider_get_value(lv_event_get_target(e));
    refresh_bg();
}

static void spinbox_inc_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_SHORT_CLICKED || code == LV_EVENT_LONG_PRESSED_REPEAT) {
        lv_spinbox_increment(spinbox);
        bright_val = lv_spinbox_get_value(spinbox);
        refresh_bg();
    }
}

static void spinbox_dec_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_SHORT_CLICKED || code == LV_EVENT_LONG_PRESSED_REPEAT) {
        lv_spinbox_decrement(spinbox);
        bright_val = lv_spinbox_get_value(spinbox);
        refresh_bg();
    }
}

/* ── public API ──────────────────────────────────────────────────────── */
void app_ui_init(void)
{
    lv_obj_t *scr = lv_scr_act();

    /* Remove default screen padding / border so the swatch fills edge-to-edge */
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_set_style_border_width(scr, 0, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    /* Apply initial background color */
    refresh_bg();

    /* ── Control panel (semi-transparent, pinned to bottom) ────────── */
    lv_obj_t *panel = lv_obj_create(scr);
    lv_obj_set_size(panel, 800, 200);
    lv_obj_align(panel, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(panel, lv_color_make(30, 30, 30), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_80, 0);
    lv_obj_set_style_radius(panel, 0, 0);
    lv_obj_set_style_border_width(panel, 0, 0);
    lv_obj_set_style_pad_hor(panel, 20, 0);
    lv_obj_set_style_pad_ver(panel, 15, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

    /* ── Title ──────────────────────────────────────────────────────── */
    lv_obj_t *title = lv_label_create(panel);
    lv_label_set_text(title, "Background Color Controller");
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 0);

    /* ── Hue slider row ─────────────────────────────────────────────── */
    lv_obj_t *hue_lbl = lv_label_create(panel);
    lv_label_set_text(hue_lbl, "Color (Hue):");
    lv_obj_set_style_text_color(hue_lbl, lv_color_white(), 0);
    lv_obj_align(hue_lbl, LV_ALIGN_TOP_LEFT, 0, 40);

    lv_obj_t *slider = lv_slider_create(panel);
    lv_slider_set_range(slider, 0, 359);
    lv_slider_set_value(slider, hue_val, LV_ANIM_OFF);
    lv_obj_set_width(slider, 530);
    lv_obj_align(slider, LV_ALIGN_TOP_RIGHT, 0, 45);
    lv_obj_add_event_cb(slider, slider_cb, LV_EVENT_VALUE_CHANGED, NULL);

    /* ── Brightness spinbox row ─────────────────────────────────────── */
    lv_obj_t *br_lbl = lv_label_create(panel);
    lv_label_set_text(br_lbl, "Brightness:");
    lv_obj_set_style_text_color(br_lbl, lv_color_white(), 0);
    lv_obj_align(br_lbl, LV_ALIGN_TOP_LEFT, 0, 110);

    spinbox = lv_spinbox_create(panel);
    lv_spinbox_set_range(spinbox, 0, 100);
    lv_spinbox_set_digit_format(spinbox, 3, 0);
    lv_spinbox_set_value(spinbox, bright_val);
    lv_obj_set_width(spinbox, 120);
    lv_obj_align(spinbox, LV_ALIGN_TOP_LEFT, 200, 100);

    lv_coord_t sb_h = lv_obj_get_height(spinbox);

    /* Decrement (–) button */
    lv_obj_t *btn_dec = lv_btn_create(panel);
    lv_obj_set_size(btn_dec, sb_h, sb_h);
    lv_obj_align_to(btn_dec, spinbox, LV_ALIGN_OUT_LEFT_MID, -5, 0);
    lv_obj_add_event_cb(btn_dec, spinbox_dec_cb, LV_EVENT_ALL, NULL);
    lv_obj_t *lbl_dec = lv_label_create(btn_dec);
    lv_label_set_text(lbl_dec, LV_SYMBOL_MINUS);
    lv_obj_center(lbl_dec);

    /* Increment (+) button */
    lv_obj_t *btn_inc = lv_btn_create(panel);
    lv_obj_set_size(btn_inc, sb_h, sb_h);
    lv_obj_align_to(btn_inc, spinbox, LV_ALIGN_OUT_RIGHT_MID, 5, 0);
    lv_obj_add_event_cb(btn_inc, spinbox_inc_cb, LV_EVENT_ALL, NULL);
    lv_obj_t *lbl_inc = lv_label_create(btn_inc);
    lv_label_set_text(lbl_inc, LV_SYMBOL_PLUS);
    lv_obj_center(lbl_inc);
}
