/*
 * mqtt_handler.c – Wi-Fi station + MQTT subscriber + JSON dispatch
 *
 * Connects to Wi-Fi using credentials in wifi_creds.h, then subscribes
 * to "Conveyor/Current_Count/Day_Total_Count".
 *
 * Expected JSON payload format:
 *   {"L1Z1": {"current": 3, "day_total": 45}, "L2Z3": {"current": 1, "day_total": 18}, ...}
 *
 * Each zone key (LxZy) is mapped to a cell index and forwarded to
 * app_ui_set_zone(zone_idx, current, day_total).
 *
 * Zone key → cell-index mapping (matches the 3×3 grid layout in ui.c):
 *   3rd Level row (top):    L3Z1=0  L3Z2=1  L3Z3=2
 *   2nd Level row (middle): L2Z3=3  L2Z2=4  L2Z1=5
 *   1st Level row (bottom): L1Z1=6  L1Z2=7  L1Z3=8
 */

#include "mqtt_handler.h"
#include "wifi_creds.h"
#include "ui.h"

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "mqtt_client.h"
#include "cJSON.h"
#include "nvs_flash.h"

#include <string.h>

#define TAG "mqtt_handler"

#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAIL_BIT       BIT1
#define WIFI_RETRY_MAX      10

static EventGroupHandle_t s_wifi_event_group;
static int s_retry_num = 0;

/* ── Zone-key → cell-index lookup ─────────────────────────────────── */
static const struct { const char *key; int idx; } s_zone_map[] = {
    {"L3Z1", 0}, {"L3Z2", 1}, {"L3Z3", 2},
    {"L2Z3", 3}, {"L2Z2", 4}, {"L2Z1", 5},
    {"L1Z1", 6}, {"L1Z2", 7}, {"L1Z3", 8},
};
#define ZONE_MAP_COUNT (int)(sizeof(s_zone_map) / sizeof(s_zone_map[0]))

static int zone_key_to_idx(const char *key)
{
    for (int i = 0; i < ZONE_MAP_COUNT; i++) {
        if (strcmp(key, s_zone_map[i].key) == 0) return s_zone_map[i].idx;
    }
    return -1;
}

/* ── Wi-Fi event handler ─────────────────────────────────────────── */
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT &&
               event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num < WIFI_RETRY_MAX) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "WiFi retry %d/%d", s_retry_num, WIFI_RETRY_MAX);
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
            ESP_LOGE(TAG, "WiFi connection failed after %d retries",
                     WIFI_RETRY_MAX);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

/* ── MQTT event handler ───────────────────────────────────────────── */
static void mqtt_event_handler(void *handler_args, esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;

    switch ((esp_mqtt_event_id_t)event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "MQTT connected – subscribing");
            esp_mqtt_client_subscribe(event->client,
                "Conveyor/Current_Count/Day_Total_Count", 0);
            break;

        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "MQTT disconnected");
            break;

        case MQTT_EVENT_DATA: {
            /* Null-terminate the incoming data for cJSON */
            char *data = strndup(event->data, (size_t)event->data_len);
            if (!data) {
                ESP_LOGE(TAG, "strndup OOM (size=%d)", event->data_len);
                break;
            }

            cJSON *root = cJSON_Parse(data);
            if (!root) {
                ESP_LOGW(TAG, "JSON parse failed for payload: %.120s", data);
                free(data);
                break;
            }
            free(data);

            cJSON *zone_obj;
            cJSON_ArrayForEach(zone_obj, root) {
                const char *key = zone_obj->string;
                if (!key) continue;

                int idx = zone_key_to_idx(key);
                if (idx < 0) {
                    ESP_LOGD(TAG, "Unknown zone key: %s", key);
                    continue;
                }

                cJSON *j_cur = cJSON_GetObjectItemCaseSensitive(zone_obj,
                                                                "current");
                cJSON *j_tot = cJSON_GetObjectItemCaseSensitive(zone_obj,
                                                                "day_total");
                if (!cJSON_IsNumber(j_cur) || !cJSON_IsNumber(j_tot)) {
                    ESP_LOGW(TAG, "Zone %s missing numeric fields", key);
                    continue;
                }

                int current   = (int)j_cur->valuedouble;
                int day_total = (int)j_tot->valuedouble;
                ESP_LOGI(TAG, "Zone %s (idx %d): current=%d day_total=%d",
                         key, idx, current, day_total);

                app_ui_set_zone(idx, current, day_total);
            }

            cJSON_Delete(root);
            break;
        }

        case MQTT_EVENT_ERROR:
            if (event->error_handle) {
                ESP_LOGE(TAG, "MQTT error: type=%d, esp_tls_last_esp_err=0x%x",
                         event->error_handle->error_type,
                         event->error_handle->esp_tls_last_esp_err);
            } else {
                ESP_LOGE(TAG, "MQTT error (no error_handle)");
            }
            break;

        default:
            break;
    }
}

/* ── Public entry point ───────────────────────────────────────────── */
void mqtt_handler_start(void)
{
    /* NVS is required by the Wi-Fi driver */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* --- Wi-Fi -------------------------------------------------------- */
    s_wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t wifi_init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wifi_init_cfg));

    esp_event_handler_instance_t inst_any_id;
    esp_event_handler_instance_t inst_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, &inst_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, &inst_got_ip));

    wifi_config_t wifi_cfg = {
        .sta = {
            .ssid               = WIFI_SSID,
            .password           = WIFI_PASSWORD,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Connecting to WiFi SSID: %s", WIFI_SSID);

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE, pdFALSE,
                                           portMAX_DELAY);
    if (!(bits & WIFI_CONNECTED_BIT)) {
        ESP_LOGE(TAG, "WiFi failed – MQTT will not start");
        return;
    }

    /* --- MQTT --------------------------------------------------------- */
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = MQTT_BROKER_URI,
    };
    esp_mqtt_client_handle_t client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID,
                                   mqtt_event_handler, NULL);
    ESP_ERROR_CHECK(esp_mqtt_client_start(client));
    ESP_LOGI(TAG, "MQTT client started, broker: %s", MQTT_BROKER_URI);
}
