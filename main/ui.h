#pragma once

/**
 * @brief Initialize the 3×3 fill-bar grid UI.
 *
 * Call once, inside an lvgl_port_lock() / lvgl_port_unlock() section.
 *
 * Creates nine cells spread evenly across the 800×480 display.
 * Each cell contains:
 *   - A horizontal fill-bar slider whose indicator colour reflects the current
 *     value.  Touch-draggable.  A shimmer stripe sweeps continuously across
 *     the bar in the fill direction, giving a "charging battery" wave effect.
 *   - Three square buttons (80×80 px, labeled 0 / 1 / 2) that control how
 *     quickly the bar automatically returns to zero:
 *       0 – no automatic decrease
 *       1 – decrease by 1 unit every 2 seconds
 *       2 – decrease by 1 unit every second
 *
 * Colour mapping: 0–20 green · 21–25 green→yellow · 26–30 yellow ·
 *                 31–35 yellow→red · 36–40 red.
 *
 * Background: soft cornflower-blue normally; flashes red/blue at 2 Hz
 * while any bar is at 40.
 */
void app_ui_init(void);
