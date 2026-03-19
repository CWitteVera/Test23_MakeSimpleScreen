#pragma once

/**
 * @brief Initialize the 3×3 fill-bar grid UI.
 *
 * Call once, inside an lvgl_port_lock() / lvgl_port_unlock() section.
 *
 * Creates nine cells spread evenly across the 800×480 display.
 * Each cell contains:
 *   - A vertical fill-bar slider whose indicator colour reflects the current
 *     brightness value.  The slider is touch-draggable.
 *   - A spinbox row (−  value  +) for entering a brightness in 0–40.
 *     Both controls are kept in sync.
 *
 * Colour mapping: 0–20 green · 21–25 green→yellow · 26–30 yellow ·
 *                 31–35 yellow→red · 36–40 red.
 *
 * Background: soft cornflower-blue normally; flashes red/blue at 2 Hz
 * while any bar is at 40.
 */
void app_ui_init(void);
