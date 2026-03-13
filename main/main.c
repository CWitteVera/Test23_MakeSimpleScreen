#include "waveshare_rgb_lcd_port.h"

void app_main(void)
{
    ESP_ERROR_CHECK(waveshare_esp32_s3_rgb_lcd_init());

    ESP_LOGI(TAG, "Display LVGL demos");
    if (lvgl_port_lock(-1)) {
        lv_demo_widgets();
        lvgl_port_unlock();
    }
}
