# Stability Checklist (ESP-IDF, Waveshare 7-inch)

Use this before major edits or after any regression.

## 1. Scope Lock
- Framework is ESP-IDF only.
- Baseline is `08_lvgl_Porting`.
- No Arduino references or pin tables.

## 2. Pin Lock
- RGB: `HSYNC=46 VSYNC=3 DE=5 PCLK=7`
- RGB data: `14,38,18,17,10,39,0,45,48,47,21,1,2,42,41,40`
- Touch: `SCL=9 SDA=8 INT=4 RST=GPIO_NUM_NC`

## 3. Timing Lock
- `pclk_hz=16MHz`
- `hsync_pulse/back/front = 4/8/8`
- `vsync_pulse/back/front = 4/8/8`
- `pclk_active_neg=true`

## 4. Config Lock
- `CONFIG_IDF_TARGET="esp32s3"`
- `CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_240=y`
- `CONFIG_FREERTOS_HZ=1000`
- `CONFIG_SPIRAM=y`
- `CONFIG_SPIRAM_MODE_OCT=y`
- `CONFIG_SPIRAM_SPEED_80M=y`
- `CONFIG_ESP32S3_DATA_CACHE_LINE_64B=y`

## 5. Dependency Lock
```yaml
dependencies:
  idf: ">=5.1.0"
  lvgl/lvgl: ">8.3.9,<9"
  espressif/esp_lcd_touch_gt911: "^1"
```

## 6. Quick Commands
```bash
idf.py set-target esp32s3
idf.py update-dependencies
idf.py menuconfig
idf.py build
idf.py -p COMx flash monitor
```

## 7. Anti Tail-Chasing Rule
- After two failed fixes for the same symptom:
  - Stop.
  - Re-run this checklist.
  - Change only one variable in the next attempt.
