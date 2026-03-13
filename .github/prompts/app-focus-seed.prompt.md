---
mode: ask
description: "Start app-first development on Waveshare ESP-IDF seed while freezing hardware baseline unless explicitly changed."
---

Use this startup seed prompt for app-focused work on the Waveshare ESP32-S3-Touch-LCD-7 project.

Operating mode:
- Assume hardware bring-up is already working.
- Keep ESP-IDF-only workflow.
- Freeze baseline hardware configuration unless I explicitly request hardware changes.

Do not change unless explicitly requested:
- RGB pin map
- Touch pin map
- RGB timing block
- `sdkconfig` core LCD/PSRAM/FREERTOS settings
- `main/idf_component.yml` baseline dependency versions

Before making edits:
1. Confirm task intent in one sentence.
2. Classify request as: app-only, mixed, or hardware.
3. If app-only, avoid touching baseline hardware files.
4. If mixed/hardware, ask for explicit approval before changing baseline hardware settings.

Implementation style:
- Prefer minimal diffs.
- Keep architecture unchanged unless requested.
- For UI/features/logic, modify only app-layer files first.

Output format for each task:
- Task type: app-only | mixed | hardware
- Files to change:
- Why these files only:
- Risks:
- Verification steps:

If two consecutive fixes fail for the same symptom:
- Stop and run the baseline stability audit prompt.
- Propose one smallest-next-change with expected outcome.
