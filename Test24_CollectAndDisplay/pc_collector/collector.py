"""
collector.py – Conveyor system PLC data collector and historian.

Reads sensor states from the PLC (defined in config/variables.json),
calculates box counts for each conveyor zone, writes history to a CSV file,
and prints a zone-count summary every display_update_interval_s seconds
(default 5 s).

PLC connection logic is intentionally left as a stub (see plc_connect() and
plc_read_sensor()). Drop in your own library (e.g. python-snap7, pycomm3,
pymodbus, etc.) once it is ready.

Usage:
    python collector.py [--config-dir PATH]

Arguments:
    --config-dir  Directory that contains plc_config.json and variables.json.
                  Defaults to the 'config' subdirectory next to this script.
"""

import argparse
import csv
import json
import logging
import os
import sys
import time
from collections import defaultdict
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Dict, List, Optional

# ---------------------------------------------------------------------------
# Logging
# ---------------------------------------------------------------------------
logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s  %(levelname)-8s  %(message)s",
    datefmt="%Y-%m-%d %H:%M:%S",
)
log = logging.getLogger("collector")

# ---------------------------------------------------------------------------
# Zone ordering (used for display and historian column order)
# ---------------------------------------------------------------------------
ZONE_ORDER: List[str] = [
    "L1Z1", "L1Z2", "L1Z3",
    "L2Z1", "L2Z2", "L2Z3",
    "L3Z1", "L3Z2", "L3Z3",
]

# ---------------------------------------------------------------------------
# Config loading
# ---------------------------------------------------------------------------

def load_json(path: Path) -> Any:
    with open(path, "r", encoding="utf-8") as fh:
        return json.load(fh)


def load_config(config_dir: Path) -> tuple:
    """Return (plc_cfg, sensors) loaded from the config directory."""
    plc_cfg = load_json(config_dir / "plc_config.json")
    var_cfg = load_json(config_dir / "variables.json")
    sensors: List[Dict] = var_cfg["sensors"]
    log.info("Loaded %d sensor definitions from variables.json", len(sensors))
    return plc_cfg, sensors

# ---------------------------------------------------------------------------
# PLC stub – replace with your library's implementation
# ---------------------------------------------------------------------------

class PlcConnection:
    """
    Stub PLC connection object.

    Replace the body of connect() and read_bool() with calls to your PLC
    communication library (e.g. python-snap7 for Siemens S7, pycomm3 for
    Allen-Bradley, pymodbus for Modbus devices, etc.).
    """

    def __init__(self, ip: str, port: int, rack: int, slot: int,
                 timeout_s: float):
        self.ip = ip
        self.port = port
        self.rack = rack
        self.slot = slot
        self.timeout_s = timeout_s
        self._connected = False

    def connect(self) -> bool:
        """
        STUB: open a connection to the PLC.
        Return True on success, False on failure.
        """
        log.warning(
            "PLC connect() is a stub – no real connection to %s:%d",
            self.ip, self.port,
        )
        # TODO: replace with e.g.:
        #   import snap7
        #   self._client = snap7.client.Client()
        #   self._client.connect(self.ip, self.rack, self.slot)
        #   self._connected = self._client.get_connected()
        self._connected = False  # stays False until real code is added
        return self._connected

    def disconnect(self) -> None:
        """STUB: close the connection."""
        self._connected = False
        log.info("PLC disconnected (stub)")

    def read_bool(self, tag: str) -> Optional[bool]:
        """
        STUB: read a single boolean (bit) from the PLC by tag/address.
        Returns True/False, or None if the read failed.
        """
        # TODO: replace with your library call, e.g.:
        #   db_num, byte_offset, bit_offset = parse_tag(tag)
        #   data = self._client.db_read(db_num, byte_offset, 1)
        #   return snap7.util.get_bool(data, 0, bit_offset)
        return None  # stub always returns None (no data)

    @property
    def connected(self) -> bool:
        return self._connected


def plc_connect(plc_cfg: Dict) -> PlcConnection:
    conn = PlcConnection(
        ip=plc_cfg["plc"]["ip"],
        port=plc_cfg["plc"]["port"],
        rack=plc_cfg["plc"]["rack"],
        slot=plc_cfg["plc"]["slot"],
        timeout_s=plc_cfg["plc"]["timeout_s"],
    )
    conn.connect()
    return conn


def plc_read_sensor(conn: PlcConnection, sensor: Dict) -> Optional[bool]:
    """
    Read one sensor's boolean value and apply the invert flag.
    Returns None if the PLC read failed.
    """
    raw = conn.read_bool(sensor["plc_tag"])
    if raw is None:
        return None
    return (not raw) if sensor.get("invert", False) else raw

# ---------------------------------------------------------------------------
# Box-count logic
# ---------------------------------------------------------------------------

class ZoneCountTracker:
    """
    Tracks box counts per conveyor zone using rising-edge detection on sensors.

    Flow summary:
      origin     sensor → +1 to zone_to
      transition sensor → +1 to zone_to, −1 from zone_from
      exit       sensor → −1 from zone_from
    """

    def __init__(self, sensors: List[Dict]):
        self.sensors = sensors
        self.counts: Dict[str, int] = {z: 0 for z in ZONE_ORDER}
        self._prev_states: Dict[str, Optional[bool]] = {
            s["name"]: None for s in sensors
        }

    def update(self, new_states: Dict[str, Optional[bool]]) -> bool:
        """
        Compare new_states against previous states.  Trigger count changes on
        rising edges (False → True).  Returns True if any count changed.
        """
        changed = False
        for sensor in self.sensors:
            name = sensor["name"]
            prev = self._prev_states.get(name)
            curr = new_states.get(name)

            # Skip if the current read failed
            if curr is None:
                self._prev_states[name] = curr
                continue

            # Rising edge: was False (or unknown), now True
            rising_edge = (prev is not True) and curr

            if rising_edge:
                sig = sensor.get("signal_type", "")
                zone_to   = sensor.get("zone_to")
                zone_from = sensor.get("zone_from")

                if sig == "origin" and zone_to:
                    self.counts[zone_to] = max(0, self.counts[zone_to] + 1)
                    log.debug("  origin: +1 → %s (now %d)", zone_to,
                              self.counts[zone_to])
                    changed = True

                elif sig == "transition":
                    if zone_to:
                        self.counts[zone_to] = max(0, self.counts[zone_to] + 1)
                        log.debug("  transition: +1 → %s (now %d)", zone_to,
                                  self.counts[zone_to])
                    if zone_from:
                        self.counts[zone_from] = max(
                            0, self.counts[zone_from] - 1
                        )
                        log.debug("  transition: -1 ← %s (now %d)", zone_from,
                                  self.counts[zone_from])
                    changed = True

                elif sig == "exit" and zone_from:
                    self.counts[zone_from] = max(0, self.counts[zone_from] - 1)
                    log.debug("  exit: -1 ← %s (now %d)", zone_from,
                              self.counts[zone_from])
                    changed = True

            self._prev_states[name] = curr

        return changed

    def snapshot(self) -> Dict[str, int]:
        """Return a copy of current zone counts."""
        return dict(self.counts)

# ---------------------------------------------------------------------------
# Historian (CSV)
# ---------------------------------------------------------------------------

class Historian:
    """
    Appends timestamped zone-count snapshots to a rotating CSV file.
    A new file is started each calendar day if rotate_daily is True.
    """

    FIELDNAMES = ["timestamp_utc"] + ZONE_ORDER + ["total_in_system"]

    def __init__(self, directory: Path, filename_prefix: str,
                 rotate_daily: bool):
        self.directory = directory
        self.filename_prefix = filename_prefix
        self.rotate_daily = rotate_daily
        self.directory.mkdir(parents=True, exist_ok=True)
        self._current_file: Optional[Path] = None
        self._writer: Optional[csv.DictWriter] = None
        self._fh = None
        self._open_for_today()

    def _today_path(self) -> Path:
        today = datetime.now(tz=timezone.utc).strftime("%Y-%m-%d")
        return self.directory / f"{self.filename_prefix}_{today}.csv"

    def _open_for_today(self) -> None:
        path = self._today_path()
        if path == self._current_file:
            return  # already open
        if self._fh:
            self._fh.close()
        need_header = not path.exists()
        self._fh = open(path, "a", newline="", encoding="utf-8")
        self._writer = csv.DictWriter(self._fh, fieldnames=self.FIELDNAMES)
        if need_header:
            self._writer.writeheader()
            self._fh.flush()
        self._current_file = path
        log.info("Historian file: %s", path)

    def write(self, counts: Dict[str, int]) -> None:
        """Write one row to the historian CSV."""
        if self.rotate_daily:
            self._open_for_today()
        row: Dict[str, Any] = {
            "timestamp_utc": datetime.now(tz=timezone.utc).isoformat(),
        }
        for zone in ZONE_ORDER:
            row[zone] = counts.get(zone, 0)
        row["total_in_system"] = sum(counts.get(z, 0) for z in ZONE_ORDER)
        self._writer.writerow(row)
        self._fh.flush()

    def close(self) -> None:
        if self._fh:
            self._fh.close()

# ---------------------------------------------------------------------------
# Display / summary
# ---------------------------------------------------------------------------

def print_zone_summary(counts: Dict[str, int]) -> None:
    """Print a formatted zone-count table to stdout."""
    now = datetime.now(tz=timezone.utc).strftime("%Y-%m-%d %H:%M:%S UTC")
    total = sum(counts.values())
    print()
    print(f"  ┌─────────────────────────────────────────────┐")
    print(f"  │  Conveyor Box Count Update  –  {now}  │")
    print(f"  ├──────────┬─────────────────────┬────────────┤")
    print(f"  │  Level   │  Zone               │  Boxes     │")
    print(f"  ├──────────┼─────────────────────┼────────────┤")
    levels = {"L1": "Level 1", "L2": "Level 2", "L3": "Level 3"}
    for prefix, label in levels.items():
        for z in (f"{prefix}Z1", f"{prefix}Z2", f"{prefix}Z3"):
            lvl = label if z.endswith("Z1") else ""
            print(f"  │  {lvl:<8}│  {z:<19}│  {counts.get(z, 0):<10}│")
        print(f"  ├──────────┼─────────────────────┼────────────┤")
    print(f"  │  {'TOTAL':<8}│  {'all zones':<19}│  {total:<10}│")
    print(f"  └──────────┴─────────────────────┴────────────┘")

# ---------------------------------------------------------------------------
# Main loop
# ---------------------------------------------------------------------------

def run(config_dir: Path) -> None:
    plc_cfg, sensors = load_config(config_dir)

    poll_interval   = plc_cfg["plc"]["poll_interval_s"]
    display_interval = plc_cfg["display_update_interval_s"]
    hist_cfg        = plc_cfg["historian"]

    historian = Historian(
        directory=config_dir.parent / hist_cfg["directory"],
        filename_prefix=hist_cfg["filename_prefix"],
        rotate_daily=hist_cfg["rotate_daily"],
    )

    tracker = ZoneCountTracker(sensors)
    conn    = plc_connect(plc_cfg)

    if not conn.connected:
        log.warning(
            "PLC is not connected (stub mode). "
            "Sensor reads will return None – counts will stay at 0 "
            "until real PLC logic is added."
        )

    last_display = 0.0

    log.info(
        "Collector running. Poll interval: %ss, Display interval: %ss",
        poll_interval, display_interval,
    )
    log.info("Press Ctrl+C to stop.")

    try:
        while True:
            loop_start = time.monotonic()

            # -- Read all sensors from PLC --------------------------------
            new_states: Dict[str, Optional[bool]] = {}
            for sensor in sensors:
                new_states[sensor["name"]] = plc_read_sensor(conn, sensor)

            # -- Update zone counts ---------------------------------------
            tracker.update(new_states)
            snapshot = tracker.snapshot()

            # -- Write to historian every poll cycle ---------------------
            historian.write(snapshot)

            # -- Print summary every display_interval seconds ------------
            now = time.monotonic()
            if now - last_display >= display_interval:
                print_zone_summary(snapshot)
                last_display = now

            # -- Sleep for remainder of poll interval --------------------
            elapsed = time.monotonic() - loop_start
            sleep_time = max(0.0, poll_interval - elapsed)
            time.sleep(sleep_time)

    except KeyboardInterrupt:
        log.info("Interrupted by user – shutting down.")
    finally:
        conn.disconnect()
        historian.close()
        log.info("Collector stopped.")


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

def main() -> None:
    parser = argparse.ArgumentParser(
        description="Conveyor PLC data collector and historian."
    )
    parser.add_argument(
        "--config-dir",
        type=Path,
        default=Path(__file__).parent / "config",
        help="Directory containing plc_config.json and variables.json "
             "(default: ./config)",
    )
    args = parser.parse_args()

    config_dir = args.config_dir.resolve()
    if not config_dir.is_dir():
        log.error("Config directory not found: %s", config_dir)
        sys.exit(1)

    run(config_dir)


if __name__ == "__main__":
    main()
