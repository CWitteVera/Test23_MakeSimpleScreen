#pragma once

/**
 * @brief Start the MQTT client and connect to the NorthPickMod broker.
 *
 * Connects to 172.20.26.49:1883 (anonymous, plain TCP).  On connection it
 * subscribes to "NorthPickMod/+/+/counts" (QoS 1).  Incoming JSON payloads
 * are parsed and routed to ui_update_zone_count().
 *
 * This function is idempotent – calling it a second time is a no-op.
 * It is called automatically by wifi_manager when an IP address is obtained,
 * so the application does not need to call it directly.
 */
void mqtt_handler_start(void);
