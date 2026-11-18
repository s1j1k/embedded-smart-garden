#!/usr/bin/env python3
"""Build plotting CSVs from Serial capture (no Influx required)."""
from __future__ import annotations

import csv
import re
from datetime import datetime, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SESSION = Path(
    (ROOT / "docs" / ".active_characterization_session").read_text(encoding="utf-8").strip()
)
P1 = SESSION / "phase1"
OUT = P1 / "plotting"
OUT.mkdir(parents=True, exist_ok=True)

RAW_CANDIDATES = [
    P1 / "serial_raw_resume.txt",
    P1 / "serial_raw.txt",
]


def utc_from_prefix(line: str) -> str | None:
    # lines may be prefixed by orchestrator as "2026-09-25T12:32:02Z >>> ..."
    m = re.match(r"(20\d{2}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}Z)", line)
    return m.group(1) if m else None


def parse_sensor_blocks(text: str) -> list[dict]:
    rows: list[dict] = []
    # Split on SENSOR DATA banners
    parts = re.split(r"={5,}\s*SENSOR DATA\s*={5,}", text)
    # Each part after the first that contains a closing ===== is a block body
    for i, part in enumerate(parts[1:], start=1):
        block = part
        # Prefer compact machine line if present
        row: dict = {"sample_idx": i}
        m = re.search(
            r"TDSppm:(\d+)\s+.*?pH:([-0-9.]+).*?PhDoseUp:(\d+)\s+PhDoseDown:(\d+)\s+.*?NutrientDose:(\d+)\s+NutrientDilute:(\d+)\s+.*?PumpOn:(\d+)\s+DrainOn:(\d+)",
            block,
            re.S,
        )
        # Compact line is usually one line — try simpler
        m2 = re.search(
            r"WaterLevelPct:(-?\d+)\s+WaterTargetPct:(\d+)\s+WaterMaxPct:(\d+)\s+WaterFloorPct:(\d+)\s+"
            r"PumpOn:(\d+)\s+DrainOn:(\d+)\s+PumpRunMs:(\d+)\s+PumpStartedMs:(\d+)\s+"
            r"TDSppm:(\d+)\s+NutrientTdsMin:(\d+)\s+NutrientTdsMax:(\d+)\s+"
            r"NutrientDose:(\d+)\s+NutrientDilute:(\d+)\s+"
            r"pH:([-0-9.]+)\s+PhTargetMin:([-0-9.]+)\s+PhTargetMax:([-0-9.]+)\s+"
            r"PhDoseUp:(\d+)\s+PhDoseDown:(\d+)\s+CO2ppm:(\d+)",
            block,
        )
        if m2:
            g = m2.groups()
            row.update(
                {
                    "water_level_pct": int(g[0]),
                    "water_target_pct": int(g[1]),
                    "pump_on": int(g[4]),
                    "drain_on": int(g[5]),
                    "pump_run_ms": int(g[6]),
                    "tds_ppm": int(g[8]),
                    "nutrient_dose": int(g[11]),
                    "nutrient_dilute": int(g[12]),
                    "ph": float(g[13]),
                    "ph_target_min": float(g[14]),
                    "ph_target_max": float(g[15]),
                    "ph_dose_up": int(g[16]),
                    "ph_dose_down": int(g[17]),
                    "co2_ppm": int(g[18]),
                }
            )
        # Human-readable fields (override / fill)
        for pat, key, cast in [
            (r"Timestamp \(ms\)\s*-\s*(\d+)", "uptime_ms", int),
            (r"System Mode\s*-\s*(\S+)", "mode", str),
            (r"Water Temperature\s*-\s*([-0-9.]+)\s*C", "water_temp_c", float),
            (r"pH Approx \(filtered\)\s*-\s*([-0-9.]+)", "ph_filtered", float),
            (r"pH Approx \(raw\)\s*-\s*([-0-9.]+)", "ph_raw", float),
            (r"pH Voltage\s*-\s*([-0-9.]+)\s*V", "ph_voltage", float),
            (r"TDS Approx\s*-\s*(\d+)\s*ppm", "tds_ppm", int),
            (r"LDR ADC\s*-\s*(\d+)", "ldr_adc", int),
            (r"Ambient Temp\s*-\s*([-0-9.]+)\s*C", "ambient_temp_c", float),
            (r"Ambient Humidity\s*-\s*([-0-9.]+)\s*%", "ambient_humidity_pct", float),
            (r"CO2\s*-\s*(\d+)\s*ppm", "co2_ppm", int),
            (r"Water Level\s*-\s*(-?\d+)\s*%", "water_level_pct", int),
        ]:
            mm = re.search(pat, block)
            if mm:
                row[key] = cast(mm.group(1))
        if "ph_filtered" in row and "ph" not in row:
            row["ph"] = row["ph_filtered"]
        if "ph" in row or "tds_ppm" in row:
            rows.append(row)
    return rows


def parse_actuations(text: str) -> list[dict]:
    rows: list[dict] = []
    for line in text.splitlines():
        ts = utc_from_prefix(line) or ""
        body = re.sub(r"^20\d{2}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}Z\s+", "", line)
        m = re.search(r"\[STAGE-CTRL\]\s+(EC dose|pH up dose|pH down dose|Top-up)\s+([-0-9.]+)\s*mL", body)
        if m:
            kind = m.group(1)
            ml = float(m.group(2))
            pump = {
                "EC dose": "nutrient",
                "pH up dose": "ph_up",
                "pH down dose": "ph_down",
                "Top-up": "water",
            }.get(kind, kind)
            rows.append(
                {
                    "ts_utc": ts,
                    "event": "dose_start",
                    "pump": pump,
                    "volume_ml": ml,
                    "line": body.strip(),
                }
            )
            continue
        m = re.search(r"\[STAGE-CTRL\]\s+(EC|pH|Top-up).*complete", body, re.I)
        if m:
            rows.append(
                {
                    "ts_utc": ts,
                    "event": "dose_complete",
                    "pump": "",
                    "volume_ml": "",
                    "line": body.strip(),
                }
            )
            continue
        m = re.search(r"\[DOSE\]\s+(\S+)\s+requested\s+([-0-9.]+)\s+mL\s+.*?delivered\s+~([-0-9.]+)\s+mL", body)
        if m:
            rows.append(
                {
                    "ts_utc": ts,
                    "event": "operator_dose",
                    "pump": m.group(1),
                    "volume_ml": float(m.group(3)),
                    "line": body.strip(),
                }
            )
            continue
        m = re.search(r"\[CTRL-LOG\]\s+(\S+)\s+stage=(\S+)\s+pH=([-0-9.]+)\s+EC=([-0-9.]+)", body)
        if m:
            rows.append(
                {
                    "ts_utc": ts,
                    "event": m.group(1),
                    "pump": "",
                    "volume_ml": "",
                    "ph": float(m.group(3)),
                    "ec": float(m.group(4)),
                    "stage": m.group(2),
                    "line": body.strip(),
                }
            )
    return rows


def write_csv(path: Path, rows: list[dict], fieldnames: list[str] | None = None) -> None:
    if not rows:
        path.write_text("", encoding="utf-8")
        return
    if fieldnames is None:
        keys: list[str] = []
        seen = set()
        for r in rows:
            for k in r:
                if k not in seen:
                    seen.add(k)
                    keys.append(k)
        fieldnames = keys
    with path.open("w", newline="", encoding="utf-8") as f:
        w = csv.DictWriter(f, fieldnames=fieldnames, extrasaction="ignore")
        w.writeheader()
        for r in rows:
            w.writerow(r)


def merge_for_plotting(sensors: list[dict], acts: list[dict]) -> list[dict]:
    """One row per sensor sample + columns marking nearby actuations."""
    out = []
    for s in sensors:
        row = dict(s)
        row["act_nutrient_ml"] = ""
        row["act_ph_up_ml"] = ""
        row["act_ph_down_ml"] = ""
        row["act_water_ml"] = ""
        row["ctrl_event"] = ""
        out.append(row)
    # Attach dose starts as separate time-series rows interleaved by sample_idx order
    # Also emit a dedicated event timeline already written separately.
    return out


def main() -> None:
    texts = []
    for p in RAW_CANDIDATES:
        if p.exists() and p.stat().st_size > 0:
            texts.append(p.read_text(encoding="utf-8", errors="replace"))
    # Also include control_events_serial.csv lines
    ce = P1 / "control_events_serial.csv"
    if ce.exists():
        texts.append(ce.read_text(encoding="utf-8", errors="replace"))

    text = "\n".join(texts)
    sensors = parse_sensor_blocks(text)
    acts = parse_actuations(text)

    # Assign synthetic timestamps from closed_loop_trace if lengths similar
    trace = P1 / "closed_loop_trace.csv"
    if trace.exists():
        with trace.open(encoding="utf-8") as f:
            troom = list(csv.DictReader(f))
        for i, s in enumerate(sensors):
            if i < len(troom) and troom[i].get("ts_utc"):
                s["ts_utc"] = troom[i]["ts_utc"]
                s["elapsed_s"] = troom[i].get("elapsed_s", "")
            # copy gains if present
            if i < len(troom):
                for g in ("gain_ph_up", "gain_ph_down", "gain_ec"):
                    if troom[i].get(g):
                        s[g] = troom[i][g]

    sensor_fields = [
        "ts_utc",
        "elapsed_s",
        "sample_idx",
        "uptime_ms",
        "mode",
        "ph",
        "ph_filtered",
        "ph_raw",
        "ph_voltage",
        "ph_target_min",
        "ph_target_max",
        "tds_ppm",
        "water_temp_c",
        "water_level_pct",
        "ldr_adc",
        "ambient_temp_c",
        "ambient_humidity_pct",
        "co2_ppm",
        "pump_on",
        "drain_on",
        "nutrient_dose",
        "nutrient_dilute",
        "ph_dose_up",
        "ph_dose_down",
        "gain_ph_up",
        "gain_ph_down",
        "gain_ec",
    ]
    write_csv(OUT / "sensors.csv", sensors, sensor_fields)
    write_csv(
        OUT / "actuations.csv",
        acts,
        ["ts_utc", "event", "pump", "volume_ml", "ph", "ec", "stage", "line"],
    )

    # Combined plot-friendly: sensors + binary dose markers aligned by nearest uptime if possible
    combined = []
    dose_flags = {"nutrient": [], "ph_up": [], "ph_down": [], "water": []}
    for a in acts:
        if a.get("event") in ("dose_start", "operator_dose") and a.get("pump") in dose_flags:
            dose_flags[a["pump"]].append(a)

    for s in sensors:
        r = {k: s.get(k, "") for k in sensor_fields}
        # Use firmware dose flags when present
        r["marker_nutrient"] = int(s.get("nutrient_dose") or 0)
        r["marker_ph_up"] = int(s.get("ph_dose_up") or 0)
        r["marker_ph_down"] = int(s.get("ph_dose_down") or 0)
        r["marker_water"] = int(s.get("pump_on") or 0)
        combined.append(r)

    write_csv(
        OUT / "sensors_actuators.csv",
        combined,
        sensor_fields
        + ["marker_nutrient", "marker_ph_up", "marker_ph_down", "marker_water"],
    )

    # Quick matplotlib plots if available
    try:
        import matplotlib

        matplotlib.use("Agg")
        import matplotlib.pyplot as plt

        if combined:
            x = list(range(len(combined)))
            ph = [float(r["ph"]) for r in combined if r.get("ph") not in ("", None)]
            tds = [float(r["tds_ppm"]) for r in combined if r.get("tds_ppm") not in ("", None)]
            fig, ax = plt.subplots(figsize=(10, 4))
            ax.plot(ph, label="pH")
            ax.axhspan(5.5, 5.8, alpha=0.15, color="green")
            ax.set_title("pH (from Serial SENSOR DATA)")
            ax.legend()
            fig.tight_layout()
            fig.savefig(OUT / "plot_ph.png", dpi=120)
            plt.close(fig)
            fig, ax = plt.subplots(figsize=(10, 4))
            ax.plot(tds, label="TDS ppm")
            ax.axhspan(800, 1200, alpha=0.15, color="green")
            ax.set_title("TDS (from Serial SENSOR DATA)")
            ax.legend()
            fig.tight_layout()
            fig.savefig(OUT / "plot_tds.png", dpi=120)
            plt.close(fig)
    except Exception as e:
        (OUT / "plot_error.txt").write_text(str(e), encoding="utf-8")

    summary = {
        "session": str(SESSION),
        "sensor_rows": len(sensors),
        "actuation_rows": len(acts),
        "outputs": [
            str(OUT / "sensors.csv"),
            str(OUT / "actuations.csv"),
            str(OUT / "sensors_actuators.csv"),
            str(OUT / "plot_ph.png"),
            str(OUT / "plot_tds.png"),
        ],
    }
    (OUT / "README.txt").write_text(
        "Local plotting extracts from Serial (Influx not required).\n"
        f"sensors.csv — {len(sensors)} rows\n"
        f"actuations.csv — {len(acts)} dose/check events\n"
        "sensors_actuators.csv — sensors + dose marker columns for overlays\n",
        encoding="utf-8",
    )
    print(summary)


if __name__ == "__main__":
    main()
