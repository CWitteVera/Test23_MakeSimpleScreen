#pragma once

/**
 * @brief Initialize the background-color-control UI.
 *
 * Call once, inside an lvgl_port_lock() / lvgl_port_unlock() section.
 *
 * The screen background color is derived from two controls:
 *   - Hue slider       (0–359) – drag to cycle through colours
 *   - Brightness entry (0–100) – tap +/- to adjust lightness
 */
void app_ui_init(void);
