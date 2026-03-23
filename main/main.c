#include "waveshare_rgb_lcd_port.h"
#include "ui.h"
#include "wifi_manager.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_event.h"

void app_main(void)
{
    ESP_ERROR_CHECK(waveshare_esp32_s3_rgb_lcd_init());

    ESP_LOGI(TAG, "Display MQTT zone-count UI");
    if (lvgl_port_lock(-1)) {
        app_ui_init();
        lvgl_port_unlock();
    }

    /* Initialize NVS – required by the WiFi driver */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* Initialize TCP/IP stack and default event loop */
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* Start WiFi station; MQTT starts automatically once an IP is obtained */
    wifi_manager_start();
}
