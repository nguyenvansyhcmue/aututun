#!/usr/bin/env python3
"""Score target ownership handoffs from a Logic grain-trace CSV.

The input is the frame-level ``logic_capture.csv`` emitted by the diagnostic
AU.  This deliberately scores renderer provenance, rather than estimating
pitch from a short mixed OLA waveform: it measures the exact interval from a
selector commit to the first dominant audible grain that owns that commit.
"""

from __future__ import annotations

import argparse
import csv
import json
import math
import statistics
from pathlib import Path
from typing import Iterable


ALIASES = {
    "sample_index": ("sample_index", "sampleIndex"),
    "target_midi": ("target_midi", "committedTargetMidi"),
    "target_changed": ("target_changed", "targetChanged"),
    "voiced": ("voiced",),
    "audible_grain_target_midi": ("audible_grain_target_midi", "olaDominantGrainTargetMidi"),
    "dry_guard_active": ("dry_guard_active", "dryGuardActive"),
    "renderer_grain_starved": ("renderer_grain_starved", "rendererGrainStarved"),
}


def number(row: dict[str, str], key: str, default: float = 0.0) -> float:
    try:
        value = float(next((row.get(name, "") for name in ALIASES.get(key, (key,)) if row.get(name, "") != ""), ""))
        return value if math.isfinite(value) else default
    except ValueError:
        return default


def integer(row: dict[str, str], key: str, default: int = -1) -> int:
    return int(round(number(row, key, default)))


def percentile(values: list[float], quantile: float) -> float:
    if not values:
        return float("nan")
    ordered = sorted(values)
    position = (len(ordered) - 1) * quantile
    lower, upper = math.floor(position), math.ceil(position)
    if lower == upper:
        return ordered[lower]
    return ordered[lower] + (ordered[upper] - ordered[lower]) * (position - lower)


def load(path: Path) -> list[dict[str, str]]:
    with path.open(newline="") as stream:
        reader = csv.DictReader(stream)
        if reader.fieldnames is None:
            raise ValueError("CSV has no header")
        missing = [key for key, names in ALIASES.items() if not any(name in reader.fieldnames for name in names)]
        if missing:
            raise ValueError("CSV lacks required telemetry: " + ", ".join(sorted(missing)))
        return list(reader)


def score(rows: list[dict[str, str]], sample_rate: float, horizon_ms: float) -> list[dict[str, object]]:
    horizon_samples = round(sample_rate * horizon_ms / 1000.0)
    results: list[dict[str, object]] = []
    previous_target = -1
    previous_changed = 0
    for index, commit in enumerate(rows):
        target = integer(commit, "target_midi")
        # ``target_changed`` may be held for several diagnostic frames. Its
        # rising edge is the commit marker; target identity is the fallback
        # for captures that do not export that level marker.
        changed = integer(commit, "target_changed")
        target_edge = target >= 0 and ((changed != 0 and previous_changed == 0)
                                       or (changed == 0 and target != previous_target))
        previous_target = target
        previous_changed = changed
        if not target_edge or integer(commit, "voiced") == 0:
            continue
        start = integer(commit, "sample_index")
        old_audible = integer(commit, "audible_grain_target_midi")
        # A voiced re-entry where there is no old wet grain belongs to the dry
        # guard test, not the OLA takeover measurement.
        if old_audible < 0:
            continue
        first_correct: dict[str, str] | None = None
        dry_guard = grain_starved = False
        for frame in rows[index:]:
            elapsed = integer(frame, "sample_index") - start
            if elapsed > horizon_samples:
                break
            dry_guard = dry_guard or integer(frame, "dry_guard_active") != 0
            grain_starved = grain_starved or integer(frame, "renderer_grain_starved") != 0
            if integer(frame, "audible_grain_target_midi") == target:
                first_correct = frame
                break
        hold_ms: float | None = None
        if first_correct is not None:
            hold_ms = (integer(first_correct, "sample_index") - start) * 1000.0 / sample_rate
        results.append(
            {
                "commit_sample": start,
                "commit_ms": start * 1000.0 / sample_rate,
                "old_audible_midi": old_audible,
                "target_midi": target,
                "old_hold_ms": hold_ms,
                "dry_guard": int(dry_guard),
                "grain_starved": int(grain_starved),
                "pass_within_horizon": int(hold_ms is not None),
            }
        )
    return results


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("capture", type=Path)
    parser.add_argument("--sample-rate", type=float, default=48000.0)
    parser.add_argument("--horizon-ms", type=float, default=80.0)
    parser.add_argument("--output-dir", type=Path, required=True)
    args = parser.parse_args()

    rows = load(args.capture)
    results = score(rows, args.sample_rate, args.horizon_ms)
    args.output_dir.mkdir(parents=True, exist_ok=True)
    metrics_path = args.output_dir / "handoff_metrics.csv"
    with metrics_path.open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=[
            "commit_sample", "commit_ms", "old_audible_midi", "target_midi",
            "old_hold_ms", "dry_guard", "grain_starved", "pass_within_horizon",
        ])
        writer.writeheader()
        writer.writerows(results)

    holds = [float(row["old_hold_ms"]) for row in results if row["old_hold_ms"] is not None]
    summary = {
        "capture": str(args.capture),
        "sample_rate": args.sample_rate,
        "horizon_ms": args.horizon_ms,
        "voiced_handoffs": len(results),
        "resolved_handoffs": len(holds),
        "old_hold_p50_ms": statistics.median(holds) if holds else None,
        "old_hold_p90_ms": percentile(holds, 0.90) if holds else None,
        "old_hold_max_ms": max(holds) if holds else None,
        "dry_guard_handoffs": sum(int(row["dry_guard"]) for row in results),
        "grain_starvation_handoffs": sum(int(row["grain_starved"]) for row in results),
        "all_resolved": len(holds) == len(results),
    }
    summary_path = args.output_dir / "summary.json"
    summary_path.write_text(json.dumps(summary, indent=2) + "\n")
    print(json.dumps(summary, sort_keys=True))
    return 0 if results and summary["all_resolved"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
