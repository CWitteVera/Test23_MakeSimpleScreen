# Test24_CollectAndDisplay

Conveyor-system data collector and display for the Waveshare ESP32-S3-Touch-LCD-7.

---

## Project layout

```
Test24_CollectAndDisplay/
├── esp32_firmware/          ESP-IDF firmware (Waveshare ESP32-S3-Touch-LCD-7)
│   ├── CMakeLists.txt
│   ├── sdkconfig.defaults
│   ├── partitions.csv
│   ├── dependencies.lock
│   └── main/
│       ├── main.c
│       ├── ui.c / ui.h      3×3 zone display (duplicated from Test23)
│       ├── lvgl_port.*
│       └── waveshare_rgb_lcd_port.*
└── pc_collector/            PC-side Python collector
    ├── collector.py         Main program
    ├── requirements.txt     Python dependencies (PLC library TBD)
    ├── config/
    │   ├── plc_config.json  PLC IP, port, poll interval, historian settings
    │   └── variables.json   Sensor variable map (PLC tags + invert flags)
    └── historian/           Auto-created CSV history files (one per day)
```

---

## Conveyor system overview

```
                   ┌──────┐   ┌──────┐   ┌──────┐
  [ORIGIN] ──────► │ L1Z1 ├──►│ L1Z2 ├──►│ L1Z3 │
                   └──────┘   └──────┘   └──┬───┘
                                             │
                   ┌──────┐   ┌──────┐   ┌──▼───┐
                   │ L2Z1 │◄──┤ L2Z2 │◄──┤ L2Z3 │
                   └──┬───┘   └──────┘   └──┬───┘
                      │                      │
                      ▼                      ▼
                 [EXIT L2]           ┌──────────────┐
                                     │  (continue)  │
                   ┌──────┐   ┌──────┐   ┌──────┐
                   │ L3Z1 ├──►│ L3Z2 ├──►│ L3Z3 │
                   └──────┘   └──────┘   └──┬───┘
                                             │
                                             ▼
                                        [EXIT L3]
```

Box flow paths:
- **Short route**: L1Z1 → L1Z2 → L1Z3 → L2Z1 → L2Z2 → L2Z3 → **EXIT L2**
- **Full route**:  L1Z1 → L1Z2 → L1Z3 → L2Z1 → L2Z2 → L2Z3 → L3Z1 → L3Z2 → L3Z3 → **EXIT L3**

---

## PC Collector

### What it does

1. Reads `config/plc_config.json` for the PLC IP address and timing settings.
2. Reads `config/variables.json` for the list of sensors to poll (including optional invert flag).
3. Polls the PLC every `poll_interval_s` seconds (default 1 s).
4. On each rising edge of a sensor signal the zone box-count is updated:
   - **origin** sensor   → increments `zone_to`
   - **transition** sensor → increments `zone_to`, decrements `zone_from`
   - **exit** sensor     → decrements `zone_from`
5. Every row is appended to a daily CSV historian file in `historian/`.
6. A formatted zone-count summary is printed every `display_update_interval_s` seconds (default 5 s).

### PLC connection (stub)

The PLC communication code is a **stub** – `PlcConnection.connect()` and
`PlcConnection.read_bool()` in `collector.py` are placeholders.  Add your PLC
library calls there once the library/logic is ready.

Suggested libraries are listed in `requirements.txt`.

### Running

```bash
cd pc_collector
pip install -r requirements.txt        # install PLC library once chosen
python collector.py                    # uses ./config by default
python collector.py --config-dir /path/to/config
```

### Configuring sensors (`variables.json`)

Each entry in the `sensors` array represents one digital sensor:

| Field         | Description |
|---------------|-------------|
| `name`        | Unique friendly name used in logs and historian |
| `plc_tag`     | PLC address / tag name (update to match your PLC) |
| `description` | Human-readable description |
| `invert`      | `true` for normally-closed (active-LOW) sensors |
| `signal_type` | `"origin"` / `"transition"` / `"exit"` (see above) |
| `zone_from`   | Zone that loses a box (transition / exit signals) |
| `zone_to`     | Zone that gains a box (origin / transition signals) |

---

## ESP32 Firmware

The firmware in `esp32_firmware/` is duplicated from **Test23_MakeSimpleScreen**
and targets the Waveshare ESP32-S3-Touch-LCD-7.

The display shows a 3×3 grid of fill-bar sliders – one per conveyor zone.
Row labels (left strip): **1st Level / 2nd Level / 3rd Level**.
Column labels: **Zone 1 / Zone 2 / Zone 3**.

### Building

```bash
cd esp32_firmware
idf.py set-target esp32s3
idf.py update-dependencies
idf.py build
idf.py -p <PORT> flash monitor
```

---

## Creating the GitHub repository

Because this project was prepared inside the Test23 repo, follow these steps
to publish it as its own repository:

```bash
# 1. Create the new repo on GitHub (CLI or web UI)
gh repo create CWitteVera/Test24_CollectAndDisplay --public

# 2. Clone it locally
git clone https://github.com/CWitteVera/Test24_CollectAndDisplay.git
cd Test24_CollectAndDisplay

# 3. Copy all files from this subdirectory into the new repo root
cp -r /path/to/Test23_MakeSimpleScreen/Test24_CollectAndDisplay/. .

# 4. Commit and push
git add .
git commit -m "Initial commit – Test24_CollectAndDisplay"
git push
```
