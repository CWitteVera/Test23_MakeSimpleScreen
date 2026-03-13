---
mode: ask
description: "Pre-change safety gate for ESP-IDF Waveshare project. Run before edits; blocks broad refactors unless baseline checks pass."
---

Run the pre-change safety gate for this repository before making code edits.

Gate intent:
- Prevent tail-chasing and uncontrolled refactors.
- Ensure current state is anchored to known-good baseline first.

Baseline sources:
- `WS_esp32Screen_DemoRules.md`
- `STABILITY_CHECKLIST.md`
- `waveshare_demo/ESP32-S3-Touch-LCD-7-Demo/ESP-IDF/08_lvgl_Porting`

Required checks (in order):
1. Scope check
- Confirm ESP-IDF-only path for this task.
- Confirm request is baseline-preserving unless user explicitly requested divergence.

2. Baseline parity check
- RGB pin map parity.
- Touch pin map parity.
- RGB timing parity.
- `sdkconfig` core settings parity.
- `main/idf_component.yml` dependency parity.

3. Change-surface check
- List files likely to change.
- Classify change size: small, medium, broad.
- If broad, explain why small change is insufficient.

4. Risk gate
- Identify top 3 regression risks.
- Define one rollback point (file(s)/setting(s) to restore first).

Decision rules:
- If any baseline parity check fails, output `BLOCKED` and propose one minimal alignment change before feature work.
- If checks pass and change size is small/medium, output `APPROVED` and provide minimal implementation plan.
- If checks pass but change is broad, output `CONDITIONAL` with staged plan and stop-after-each-stage verification.

Output format:
- Gate result: `APPROVED` | `CONDITIONAL` | `BLOCKED`
- Scope check result:
- Baseline parity results:
  - RGB pins:
  - Touch pins:
  - RGB timing:
  - sdkconfig core:
  - dependencies:
- Planned change surface:
- Risk summary (top 3):
- Rollback point:
- Next smallest action:
- Expected outcome:

Constraints:
- Do not suggest architecture replacement unless explicitly requested.
- Prefer minimal diffs.
- If blocked, do not produce implementation steps beyond the single alignment fix.
