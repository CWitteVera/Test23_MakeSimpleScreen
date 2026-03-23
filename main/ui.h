#pragma once

/**
 * @brief Initialize the 3×3 fill-bar grid UI.
 *
 * Call once, inside an lvgl_port_lock() / lvgl_port_unlock() section.
 *
 * Creates nine cells spread evenly across the 800×480 display.
 * Each cell contains:
 *   - A horizontal fill-bar slider whose indicator colour reflects the current
 *     value.  A shimmer stripe sweeps continuously across the bar in the fill
 *     direction, giving a "charging battery" wave effect.
 *   - A numeric count label that shows the last value received via MQTT.
 *     When no update has arrived for ≥10 seconds the label shows "---" in grey.
 *
 * Colour mapping: 0–20 green · 21–25 green→yellow · 26–30 yellow ·
 *                 31–35 yellow→red · 36–40 red.
 *
 * Background: soft cornflower-blue normally; flashes red/blue at 2 Hz
 * while any bar is at 40.
 */
void app_ui_init(void);

/**
 * @brief Update the fill-bar and count label for a specific level/zone.
 *
 * Intended to be called from the MQTT handler task (or any FreeRTOS task).
 * Acquires the LVGL mutex internally; no external locking required.
 *
 * @param level  1-based level index (1 = 1st Level, … 3 = 3rd Level).
 * @param zone   1-based zone index  (1 … 3).
 * @param count  Raw count value received from the broker.  The bar is clamped
 *               to [0, 40]; the count label shows the raw integer.
 */
void ui_update_zone_count(int level, int zone, int count);
