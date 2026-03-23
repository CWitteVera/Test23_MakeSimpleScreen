#pragma once

/**
 * @brief Start WiFi in station mode and connect to the configured AP.
 *
 * Reads SSID and password from Kconfig (CONFIG_MQTT_WIFI_SSID /
 * CONFIG_MQTT_WIFI_PASSWORD).  When an IP address is obtained the MQTT
 * handler is started automatically.  Reconnect on disconnect is handled
 * internally via the ESP event loop.
 *
 * Prerequisites: nvs_flash_init(), esp_netif_init(), and
 *                esp_event_loop_create_default() must all have been called
 *                before this function.
 */
void wifi_manager_start(void);
