#pragma once

/**
 * @brief Initialize the 3×3 fill-bar grid UI.
 *
 * Call once, inside an lvgl_port_lock() / lvgl_port_unlock() section.
 *
 * Creates nine cells spread evenly across the 800×480 display.
 * Each cell contains:
 *   - A zone label.
 *   - A horizontal fill-bar (read-only) whose indicator colour reflects the
 *     current value.  A shimmer stripe sweeps across the fill, giving a
 *     "charging battery" wave effect.
 *   - Two text boxes labeled "Current" and "Total" that display the latest
 *     values pushed via app_ui_set_zone().
 *
 * Colour mapping: 0–20 green · 21–25 green→yellow · 26–30 yellow ·
 *                 31–35 yellow→red · 36–40 red.
 *
 * Background: soft cornflower-blue normally; flashes red/blue at 2 Hz
 * while any bar is at 40.
 */
void app_ui_init(void);

/**
 * @brief Update one zone cell from MQTT data.
 *
 * Thread-safe – may be called from any FreeRTOS task.
 * Acquires the LVGL port lock internally.
 *
 * The fill bar is set to min(current, 40).
 * The "Current" text box shows @p current and "Total" shows @p day_total.
 *
 * Zone-index → grid-cell mapping (matches mqtt_handler.c zone_map):
 *   3rd Level row (top):    L3Z1=0  L3Z2=1  L3Z3=2
 *   2nd Level row (middle): L2Z3=3  L2Z2=4  L2Z1=5
 *   1st Level row (bottom): L1Z1=6  L1Z2=7  L1Z3=8
 *
 * @param zone_idx  Cell index 0–8.
 * @param current   Current count from MQTT payload.
 * @param day_total Day-total count from MQTT payload.
 */
void app_ui_set_zone(int zone_idx, int current, int day_total);
