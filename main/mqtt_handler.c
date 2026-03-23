#include "mqtt_handler.h"
#include "ui.h"
#include "esp_log.h"
#include "mqtt_client.h"
#include "cJSON.h"
#include <stdlib.h>
#include <string.h>

#define TAG_MQTT "mqtt_hdlr"

/* Broker connection settings (from problem spec) */
#define MQTT_BROKER_URI          "mqtt://172.20.26.49:1883"
#define MQTT_KEEPALIVE_SEC       60
#define MQTT_RECONNECT_TIMEOUT_MS 5000

/* Wildcard subscription – matches NorthPickMod/level<N>/zone<M>/counts */
#define MQTT_SUBSCRIBE_TOPIC     "NorthPickMod/+/+/counts"

static esp_mqtt_client_handle_t s_mqtt_client = NULL;

/* ── helpers ────────────────────────────────────────────────────────── */

/*
 * Copy a length-bounded MQTT data field into a freshly malloc'd,
 * null-terminated C string.  Returns NULL on allocation failure.
 * Caller is responsible for free()ing the result.
 */
static char *copy_as_cstring(const char *data, int len)
{
    if (!data || len < 0) return NULL;
    char *buf = malloc((size_t)len + 1);
    if (!buf) return NULL;
    memcpy(buf, data, (size_t)len);
    buf[len] = '\0';
    return buf;
}

/* ── event handler ──────────────────────────────────────────────────── */

static void mqtt_event_handler(void *arg, esp_event_base_t event_base,
                                int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;

    switch ((esp_mqtt_event_id_t)event_id) {

    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG_MQTT, "Connected to broker");
        esp_mqtt_client_subscribe(s_mqtt_client, MQTT_SUBSCRIBE_TOPIC, 1);
        ESP_LOGI(TAG_MQTT, "Subscribed to \"%s\"", MQTT_SUBSCRIBE_TOPIC);
        break;

    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG_MQTT, "Disconnected from broker (auto-reconnect active)");
        break;

    case MQTT_EVENT_DATA: {
        char *payload = copy_as_cstring(event->data, event->data_len);
        if (!payload) {
            ESP_LOGE(TAG_MQTT, "Out of memory for payload buffer");
            break;
        }

        cJSON *root = cJSON_Parse(payload);
        free(payload);

        if (!root) {
            ESP_LOGW(TAG_MQTT, "Malformed JSON payload – ignoring");
            break;
        }

        cJSON *j_level = cJSON_GetObjectItemCaseSensitive(root, "level");
        cJSON *j_zone  = cJSON_GetObjectItemCaseSensitive(root, "zone");
        cJSON *j_count = cJSON_GetObjectItemCaseSensitive(root, "current_count");

        if (cJSON_IsNumber(j_level) &&
            cJSON_IsNumber(j_zone)  &&
            cJSON_IsNumber(j_count)) {
            int level = (int)j_level->valuedouble;
            int zone  = (int)j_zone->valuedouble;
            int count = (int)j_count->valuedouble;
            ESP_LOGI(TAG_MQTT, "Level %d Zone %d count=%d", level, zone, count);
            ui_update_zone_count(level, zone, count);
        } else {
            ESP_LOGW(TAG_MQTT, "JSON missing required fields – ignoring");
        }

        cJSON_Delete(root);
        break;
    }

    case MQTT_EVENT_ERROR:
        ESP_LOGE(TAG_MQTT, "MQTT error event");
        break;

    default:
        break;
    }
}

/* ── public API ─────────────────────────────────────────────────────── */

void mqtt_handler_start(void)
{
    if (s_mqtt_client != NULL) {
        /* Already initialised; reconnect is handled automatically by the
         * client task – nothing more to do.                              */
        return;
    }

    esp_mqtt_client_config_t cfg = {
        .broker.address.uri              = MQTT_BROKER_URI,
        .session.keepalive               = MQTT_KEEPALIVE_SEC,
        .network.reconnect_timeout_ms    = MQTT_RECONNECT_TIMEOUT_MS,
    };

    s_mqtt_client = esp_mqtt_client_init(&cfg);
    if (!s_mqtt_client) {
        ESP_LOGE(TAG_MQTT, "Failed to create MQTT client");
        return;
    }

    esp_mqtt_client_register_event(s_mqtt_client, ESP_EVENT_ANY_ID,
                                   mqtt_event_handler, NULL);
    esp_mqtt_client_start(s_mqtt_client);

    ESP_LOGI(TAG_MQTT, "MQTT client started (broker: %s)", MQTT_BROKER_URI);
}
