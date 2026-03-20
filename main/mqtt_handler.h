#pragma once

/**
 * @brief Start the Wi-Fi station and MQTT client.
 *
 * Connects to the network using credentials from wifi_creds.h, then
 * subscribes to Conveyor/Current_Count/Day_Total_Count.
 *
 * On each message, parses the JSON payload and calls app_ui_set_zone()
 * for every zone key found.
 *
 * Call once from app_main(), after app_ui_init().
 */
void mqtt_handler_start(void);
