# WS_esp32Screen_DemoRules
## Waveshare ESP32-S3-Touch-LCD-7 — ESP-IDF Strict Profile

> Board: Waveshare ESP32-S3-Touch-LCD-7 (800x480 RGB, GT911 touch)
> 
> Scope: ESP-IDF only
> 
> Baseline: `ESP-IDF/08_lvgl_Porting` demo, verified working on this unit
> 
> Last Updated: 2026-03-11

---

## 1. Scope Rules

- Use only ESP-IDF references and code paths.
- Treat `ESP-IDF/08_lvgl_Porting` as the primary truth for this project.
- Reject Arduino-specific guidance, APIs, pin maps, and setup procedures.
- If a new source conflicts with the verified demo, keep demo behavior unless hardware testing proves otherwise.

---

## 2. Verified Pin Map (From Working Demo)

### 2.1 RGB Interface

| Signal | GPIO |
|---|---|
| HSYNC | 46 |
| VSYNC | 3 |
| DE | 5 |
| PCLK | 7 |
| D0 | 14 |
| D1 | 38 |
| D2 | 18 |
| D3 | 17 |
| D4 | 10 |
| D5 | 39 |
| D6 | 0 |
| D7 | 45 |
| D8 | 48 |
| D9 | 47 |
| D10 | 21 |
| D11 | 1 |
| D12 | 2 |
| D13 | 42 |
| D14 | 41 |
| D15 | 40 |

### 2.2 Touch Interface (GT911)

| Signal | GPIO |
|---|---|
| I2C SCL | 9 |
| I2C SDA | 8 |
| INT | 4 |
| RST | GPIO_NUM_NC |

Notes:
- GT911 default address path is `ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG()`.
- In this demo flow, backlight and some control sequencing are through CH422G I2C writes.

---

## 3. Project Baseline

Use these demo files as implementation references:

- `waveshare_demo/ESP32-S3-Touch-LCD-7-Demo/ESP-IDF/08_lvgl_Porting/main/main.c`
- `waveshare_demo/ESP32-S3-Touch-LCD-7-Demo/ESP-IDF/08_lvgl_Porting/main/waveshare_rgb_lcd_port.c`
- `waveshare_demo/ESP32-S3-Touch-LCD-7-Demo/ESP-IDF/08_lvgl_Porting/main/waveshare_rgb_lcd_port.h`
- `waveshare_demo/ESP32-S3-Touch-LCD-7-Demo/ESP-IDF/08_lvgl_Porting/main/lvgl_port.c`
- `waveshare_demo/ESP32-S3-Touch-LCD-7-Demo/ESP-IDF/08_lvgl_Porting/sdkconfig.defaults`

---

## 4. Dependency Rules

`main/idf_component.yml` baseline:

```yaml
dependencies:
  idf: ">=5.1.0"
  lvgl/lvgl: ">8.3.9,<9"
  espressif/esp_lcd_touch_gt911: "^1"
```

Rules:
- Do not add `esp_lvgl_port` unless intentionally migrating away from local `lvgl_port.c`.
- Keep LVGL major version aligned with demo (`v8` range).

---

## 5. Key Runtime Config

Required baseline from verified demo:

- `CONFIG_IDF_TARGET="esp32s3"`
- `CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_240=y`
- `CONFIG_FREERTOS_HZ=1000`
- `CONFIG_SPIRAM=y`
- `CONFIG_SPIRAM_MODE_OCT=y`
- `CONFIG_SPIRAM_SPEED_80M=y`
- `CONFIG_ESP32S3_DATA_CACHE_LINE_64B=y`

Recommended for stability/perf:

- `CONFIG_SPIRAM_RODATA=y`
- `CONFIG_LCD_RGB_RESTART_IN_VSYNC=y`

---

## 6. Display Bring-Up Rules

### 6.1 RGB Timing Baseline

Use verified working values first:

- `pclk_hz = 16 * 1000 * 1000`
- `hsync_pulse_width = 4`
- `hsync_back_porch = 8`
- `hsync_front_porch = 8`
- `vsync_pulse_width = 4`
- `vsync_back_porch = 8`
- `vsync_front_porch = 8`
- `pclk_active_neg = true`

### 6.2 Backlight and Control Path

Use CH422G I2C control path from demo, not LEDC GPIO PWM baseline.

Reference sequence pattern:

```c
uint8_t write_buf = 0x01;
i2c_master_write_to_device(I2C_NUM_0, 0x24, &write_buf, 1, pdMS_TO_TICKS(1000));
write_buf = 0x1E;
i2c_master_write_to_device(I2C_NUM_0, 0x38, &write_buf, 1, pdMS_TO_TICKS(1000));
```

---

## 7. Touch Rules

- Initialize I2C on `SCL=9`, `SDA=8`, `400kHz`.
- Use `ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG()`.
- Use `int_gpio_num = 4` and `rst_gpio_num = GPIO_NUM_NC` for demo-equivalent path.
- Always call `esp_lcd_touch_read_data()` before coordinate fetch.

---

## 8. LVGL Rules

- Use local demo LVGL integration (`main/lvgl_port.c`), not a different port architecture.
- Keep `lvgl_port_lock()/lvgl_port_unlock()` around `lv_*` calls outside LVGL task context.
- Keep anti-tearing mode and buffer settings aligned with demo defaults unless profiling requires change.

Baseline app flow:

```c
ESP_ERROR_CHECK(waveshare_esp32_s3_rgb_lcd_init());
if (lvgl_port_lock(-1)) {
    lv_demo_widgets();
    lvgl_port_unlock();
}
```

---

## 9. USB/JTAG Rule

- Touch pins in this baseline are not on GPIO 19/20.
- USB Serial/JTAG can stay enabled.
- Only disable USB Serial/JTAG if a future, intentional pin remap requires it.

---

## 10. Build and Flash Commands

```bash
idf.py set-target esp32s3
idf.py update-dependencies
idf.py menuconfig
idf.py build
idf.py -p COMx flash monitor
```

---

## 11. Troubleshooting (Strict Baseline)

| Symptom | First Check |
|---|---|
| LCD no image | Verify exact RGB pin map and timing baseline from Section 2/6 |
| Backlight off | Verify CH422G I2C sequence executes successfully |
| Touch dead | Verify I2C on 9/8, INT on 4, and GT911 panel IO creation |
| Tearing/drift | Ensure VSYNC restart config and demo buffer mode are intact |
| LVGL crash | Verify all cross-task LVGL calls are mutex-guarded |

---

## 12. Change Control Rules

- Any change away from `08_lvgl_Porting` baseline must be tested on hardware.
- Record changed pins/timing/config in this file immediately after validation.
- Do not import undocumented snippets from non-ESP-IDF sources.

---

End of strict profile.
