#include "waveshare_rgb_lcd_port.h"
#include "ui.h"

void app_main(void)
{
    ESP_ERROR_CHECK(waveshare_esp32_s3_rgb_lcd_init());

    ESP_LOGI(TAG, "Display color-control UI");
    if (lvgl_port_lock(-1)) {
        app_ui_init();
        lvgl_port_unlock();
    }
}
