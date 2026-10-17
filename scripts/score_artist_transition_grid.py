#!/usr/bin/env python3
"""Score offline Vibe transition candidates against an Artist bounce.

The comparison keeps one fixed sample offset for every candidate. This is
important: allowing each candidate to choose a new alignment would turn a
slower transition into an apparent latency change and hide the parameter we
are trying to identify.
"""

import argparse
import csv
import math
from pathlib import Path

import numpy as np


def contour(path):
    values = {}
    with open(path, newline="") as stream:
        for row in csv.DictReader(stream):
            hz = float(row["independent_f0_hz"])
            if math.isfinite(hz) and hz > 0.0:
                values[int(row["sample"])] = 1200.0 * math.log2(hz / 440.0)
    return values


def percentile(values, q):
    return float(np.percentile(np.asarray(values), q)) if values else math.nan


def wide_windows(path, tail_samples):
    windows = []
    previous_key = None
    with open(path, newline="") as stream:
        for row in csv.DictReader(stream):
            if row["trajectoryTransitionActive"] != "1":
                previous_key = None
                continue
            start = float(row["trajectoryTransitionStartCents"])
            end = float(row["trajectoryTransitionEndCents"])
            if not math.isfinite(start) or not math.isfinite(end) or abs(end - start) < 149.0:
                previous_key = None
                continue
            key = (round(start, 3), round(end, 3))
            if key != previous_key:
                sample = int(row["sampleIndex"])
                windows.append((sample, sample + tail_samples, start, end))
            previous_key = key
    return windows


def errors_in_windows(artist, candidate, offset, windows=None):
    errors = []
    for sample, expected in artist.items():
        if windows is not None and not any(first <= sample < last for first, last, _, _ in windows):
            continue
        actual = candidate.get(sample + offset)
        if actual is not None:
            errors.append(actual - expected)
    return errors


def metrics(errors):
    absolute = [abs(value) for value in errors]
    trimmed = [value for value in errors if abs(value) <= 300.0]
    return {
        "frames": len(errors),
        "mae": float(np.mean(np.abs(trimmed))) if trimmed else math.nan,
        "rmse": float(np.sqrt(np.mean(np.square(trimmed)))) if trimmed else math.nan,
        "p50": percentile(absolute, 50),
        "p90": percentile(absolute, 90),
        "p95": percentile(absolute, 95),
    }


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("artist_contour", type=Path)
    parser.add_argument("candidate_root", type=Path)
    parser.add_argument("output_csv", type=Path)
    parser.add_argument("--offset", type=int, default=1280)
    parser.add_argument("--transition-ms", type=float, default=120.0)
    args = parser.parse_args()

    artist = contour(args.artist_contour)
    rows = []
    tail = round(args.transition_ms * 48.0)
    for directory in sorted(path for path in args.candidate_root.iterdir() if path.is_dir()):
        candidate = contour(directory / "contour.csv")
        windows = wide_windows(directory / "full_default_intervals.csv", tail)
        whole = metrics(errors_in_windows(artist, candidate, args.offset))
        transition = metrics(errors_in_windows(artist, candidate, args.offset, windows))
        parts = directory.name.split("-")
        shape = int(parts[1])
        duration = float(parts[3])
        downward_duration = float(parts[5]) if len(parts) >= 6 and parts[4] == "down" else duration
        # Transition RMSE is the identification objective. Whole-file RMSE is
        # retained as a guard against improving an event while harming the
        # rest of the phrase.
        score = transition["rmse"] + 0.20 * whole["rmse"]
        rows.append({"shape": shape, "up_duration_ms": duration,
                     "down_duration_ms": downward_duration,
                     "wide_events": len(windows), "score": score,
                     **{f"transition_{key}": value for key, value in transition.items()},
                     **{f"whole_{key}": value for key, value in whole.items()}})

    rows.sort(key=lambda row: row["score"])
    args.output_csv.parent.mkdir(parents=True, exist_ok=True)
    with open(args.output_csv, "w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=rows[0].keys())
        writer.writeheader()
        writer.writerows(rows)
    for row in rows[:12]:
        print(f"shape={row['shape']} up={row['up_duration_ms']:.2f} "
              f"down={row['down_duration_ms']:.2f} "
              f"score={row['score']:.4f} transition_rmse={row['transition_rmse']:.4f} "
              f"transition_p90={row['transition_p90']:.4f} whole_rmse={row['whole_rmse']:.4f}")


if __name__ == "__main__":
    main()
