#!/usr/bin/env python3
"""Compare target and rendered F0 contours at one fixed renderer latency.

The contour exporter writes a 256-sample grid. Production latency (1152
samples) is not a multiple of that grid, so an exact sample join produces no
matches. This evaluates candidate cents at target_sample + fixed_latency by
linear interpolation and never searches a different offset per target.
"""

from __future__ import annotations

import argparse
import csv
import math
from pathlib import Path


def read_cents(path: Path) -> list[tuple[int, float]]:
    result: list[tuple[int, float]] = []
    with path.open(newline="", encoding="utf-8") as stream:
        for row in csv.DictReader(stream):
            hz = float(row["independent_f0_hz"])
            if math.isfinite(hz) and hz > 0.0:
                result.append((int(row["sample"]), 1200.0 * math.log2(hz / 440.0)))
    return result


def interpolate(points: list[tuple[int, float]], sample: int) -> float | None:
    if len(points) < 2 or sample < points[0][0] or sample > points[-1][0]:
        return None
    low, high = 0, len(points) - 1
    while low + 1 < high:
        middle = (low + high) // 2
        if points[middle][0] <= sample:
            low = middle
        else:
            high = middle
    first_sample, first_value = points[low]
    if first_sample == sample:
        return first_value
    last_sample, last_value = points[high]
    amount = (sample - first_sample) / (last_sample - first_sample)
    return first_value + (last_value - first_value) * amount


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("target_contour", type=Path)
    parser.add_argument("candidate_contour", type=Path)
    parser.add_argument("latency_samples", type=int)
    parser.add_argument("--frames", type=Path)
    args = parser.parse_args()

    target = read_cents(args.target_contour)
    candidate = read_cents(args.candidate_contour)
    rows: list[tuple[int, int, float, float, float]] = []
    for sample, expected in target:
        output_sample = sample + args.latency_samples
        actual = interpolate(candidate, output_sample)
        if actual is not None:
            rows.append((sample, output_sample, expected, actual, actual - expected))

    if args.frames:
        args.frames.parent.mkdir(parents=True, exist_ok=True)
        with args.frames.open("w", newline="", encoding="utf-8") as stream:
            writer = csv.writer(stream)
            writer.writerow(("target_sample", "candidate_sample", "target_cents", "candidate_cents", "error_cents"))
            writer.writerows(rows)

    if not rows:
        print("f0_rmse_cents=nan f0_mae_cents=nan f0_matched_frames=0")
        return 1
    errors = [row[-1] for row in rows]
    rmse = math.sqrt(sum(value * value for value in errors) / len(errors))
    mae = sum(abs(value) for value in errors) / len(errors)
    print(f"f0_rmse_cents={rmse:.6f} f0_mae_cents={mae:.6f} f0_matched_frames={len(rows)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
