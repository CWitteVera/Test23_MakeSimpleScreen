# Copilot Instructions: ESP-IDF Stability Contract

## Project Profile
- Target board: `Waveshare ESP32-S3-Touch-LCD-7`
- Framework: `ESP-IDF only`
- Authoritative baseline: `WS_esp32Screen_DemoRules.md` and `STABILITY_CHECKLIST.md`
- External reference (optional): Waveshare `08_lvgl_Porting` demo

## Non-Negotiable Constraints
- Do not use Arduino guidance, APIs, examples, or pin maps.
- Do not replace architecture unless explicitly requested.
- Prefer minimal diffs over broad refactors.
- Keep pin/timing/config aligned with baseline unless hardware-tested changes are requested.

## Tail-Chasing Guardrails
- If 2 consecutive fixes fail for the same symptom, stop and run a baseline audit.
- Baseline audit must verify:
  - RGB pin map
  - Touch pin map
  - RGB timing block
  - `sdkconfig` core settings (CPU/PSRAM/FREERTOS/LCD)
  - `idf_component.yml` dependency versions
- After audit, propose one smallest-next-change and expected outcome before editing.

## Session Start Checklist
- Read `WS_esp32Screen_DemoRules.md` first.
- Confirm whether user wants strict baseline or intentional divergence.
- When in doubt, preserve currently working demo-equivalent behavior.

## Definition of Done
- Build path is still valid.
- No pin map drift without explicit approval.
- Changed files include concise rationale.
- Remaining risks are called out clearly.
