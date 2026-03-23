idf_component_register(
    SRCS "main.c" "waveshare_rgb_lcd_port.c" "lvgl_port.c" "ui.c"
         "wifi_manager.c" "mqtt_handler.c"
    INCLUDE_DIRS "."
    REQUIRES esp_wifi esp_netif esp_event esp_mqtt nvs_flash json)

idf_component_get_property(lvgl_lib lvgl__lvgl COMPONENT_LIB)
target_compile_options(${lvgl_lib} PRIVATE -Wno-format)
