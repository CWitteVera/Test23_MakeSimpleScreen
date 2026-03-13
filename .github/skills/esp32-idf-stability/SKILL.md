---
name: esp32-idf-stability
description: "Use when ESP-IDF Waveshare LCD project is unstable, regressing, or tail-chasing. Keywords: esp32-s3-touch-lcd-7, no display, touch dead, tearing, drift, regression, stabilize, baseline audit, pin mismatch, sdkconfig mismatch."
---

# ESP-IDF Stability Skill

## Goal
Return the project to a known-good state quickly by enforcing a baseline-first workflow.

## Baseline Source of Truth
- `WS_esp32Screen_DemoRules.md`
- `waveshare_demo/ESP32-S3-Touch-LCD-7-Demo/ESP-IDF/08_lvgl_Porting`

## Procedure
1. Verify current state against baseline before changing code.
2. Identify exactly one highest-probability mismatch.
3. Apply the smallest possible fix.
4. Re-check the same symptom.
5. Repeat only if a new mismatch is proven.

## Baseline Audit Items
- Pin map:
  - RGB: `46,3,5,7,14,38,18,17,10,39,0,45,48,47,21,1,2,42,41,40`
  - Touch: `SCL=9`, `SDA=8`, `INT=4`, `RST=GPIO_NUM_NC`
- RGB timings:
  - `pclk=16MHz`, `h/v pulse=4`, `h/v back/front porch=8`, `pclk_active_neg=true`
- Core config:
  - `IDF_TARGET=esp32s3`, `CPU=240MHz`, `FREERTOS_HZ=1000`, octal PSRAM enabled
- Dependencies:
  - `lvgl/lvgl >8.3.9,<9`
  - `espressif/esp_lcd_touch_gt911 ^1`

## Stop Conditions
- If 2 attempts on same symptom fail, stop and summarize:
  - What was checked
  - What still mismatches
  - Next single experiment with predicted result

## Output Template
- Symptom:
- Mismatch found:
- Minimal change made:
- Verification result:
- Residual risk:
- Next action:
