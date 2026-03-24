#pragma once

#include <stdbool.h>

/**
 * @brief Initialize the 3×3 fill-bar grid UI.
 *
 * Call once, inside an lvgl_port_lock() / lvgl_port_unlock() section.
 *
 * Layout (800×480):
 *   - A 30 px header bar at the top shows the title "North Pick Mod" on the
 *     left and a colour-coded WiFi symbol on the right.  Tapping the WiFi
 *     symbol opens a pop-up showing IP address, MQTT status, UPD count and
 *     RX count.  WiFi symbol colour: green = WiFi+MQTT, red = WiFi only,
 *     grey = not connected.
 *   - Nine cells fill the remaining 450 px in a 3×3 grid.  Each cell shows
 *     a zone label, a horizontal fill-bar slider (shimmer "charging battery"
 *     wave), and a numeric count label (shows "---" after ≥10 s without data).
 *
 * Colour mapping: 0–20 green · 21–25 green→yellow · 26–30 yellow ·
 *                 31–35 yellow→red · 36–40 red.
 *
 * Background: soft cornflower-blue normally; flashes red at 2 Hz while any
 * bar is at 40.
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

/**
 * @brief Update the WiFi status indicator in the header bar.
 *
 * Safe to call from any FreeRTOS task; acquires the LVGL mutex internally.
 * Updates the WiFi icon colour: green when both WiFi and MQTT are connected,
 * red when WiFi is connected but MQTT is not, grey when disconnected.
 *
 * @param connected  true once a valid IP address has been obtained.
 * @param ip_str     Null-terminated IP string (e.g. "192.168.1.5"), or NULL
 *                   when not connected.
 */
void ui_set_wifi_status(bool connected, const char *ip_str);

/**
 * @brief Update the MQTT broker connection indicator.
 *
 * Safe to call from any FreeRTOS task; acquires the LVGL mutex internally.
 * Triggers a re-colour of the header WiFi icon.
 *
 * @param connected  true while the MQTT client has an active broker session.
 */
void ui_set_mqtt_status(bool connected);

/**
 * @brief Signal that an MQTT message was just received from the publisher.
 *
 * Increments the diagnostic RX counter displayed in the connection-info popup.
 * Safe to call from any FreeRTOS task.
 */
void ui_notify_mqtt_rx(void);
