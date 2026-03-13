---
mode: ask
description: "Run ESP-IDF baseline stability audit for Waveshare 7-inch project (tail-chasing guardrail)."
---

Run a strict baseline audit for this repository.

Requirements:
- Use only ESP-IDF baseline sources.
- Compare current project files against:
  - `WS_esp32Screen_DemoRules.md`
  - `waveshare_demo/ESP32-S3-Touch-LCD-7-Demo/ESP-IDF/08_lvgl_Porting`
- Check, in order:
  1. RGB pin map
  2. Touch pin map
  3. RGB timing block
  4. `sdkconfig` core settings
  5. `main/idf_component.yml` dependency versions

Output format:
- Symptom under review
- Verified matches
- Mismatches found
- Single smallest-next-change
- Expected outcome
- Risk if unchanged

Important behavior:
- If no mismatch is found, say that explicitly.
- Do not propose architecture changes unless explicitly requested.
- Prefer minimal diffs.
