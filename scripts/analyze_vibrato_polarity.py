#!/usr/bin/env python3
"""Report depth/polarity/phase metrics for fixed-latency pitch contours.

This is an offline calibration utility.  It never chooses a latency per case:
candidate contour is interpolated at target_sample + fixed_latency_samples.
"""
import argparse
import csv
import math
from pathlib import Path


def load(path: Path):
    rows = []
    with path.open(newline="") as handle:
        for row in csv.DictReader(handle):
            hz = float(row["independent_f0_hz"])
            if math.isfinite(hz) and hz > 0.0:
                rows.append((float(row["sample"]), 1200.0 * math.log2(hz)))
    return rows


def interpolate(rows, sample):
    if len(rows) < 2 or sample < rows[0][0] or sample > rows[-1][0]:
        return None
    lo, hi = 0, len(rows) - 1
    while hi - lo > 1:
        mid = (lo + hi) // 2
        if rows[mid][0] <= sample:
            lo = mid
        else:
            hi = mid
    x0, y0 = rows[lo]
    x1, y1 = rows[hi]
    return y0 if x1 == x0 else y0 + (y1 - y0) * (sample - x0) / (x1 - x0)


def median(values):
    values = sorted(values)
    n = len(values)
    return values[n // 2] if n % 2 else 0.5 * (values[n // 2 - 1] + values[n // 2])


def rms(values):
    return math.sqrt(sum(x * x for x in values) / len(values)) if values else math.nan


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("target", type=Path)
    parser.add_argument("candidate", type=Path)
    parser.add_argument("latency", type=float)
    args = parser.parse_args()
    target, candidate = load(args.target), load(args.candidate)
    aligned = [(t, c) for sample, t in target
               if (c := interpolate(candidate, sample + args.latency)) is not None]
    if len(aligned) < 16:
        raise SystemExit("not enough fixed-latency voiced contour frames")
    target_center = median([x for x, _ in aligned])
    candidate_center = median([x for _, x in aligned])
    target_res = [x - target_center for x, _ in aligned]
    candidate_res = [x - candidate_center for _, x in aligned]
    errors = [c - t for t, c in aligned]

    def side(values, positive):
        side_values = [x for x in values if (x >= 0.0) == positive]
        return rms(side_values), median(side_values) if side_values else math.nan

    target_up, _ = side(target_res, True)
    target_down, _ = side(target_res, False)
    candidate_up, _ = side(candidate_res, True)
    candidate_down, _ = side(candidate_res, False)
    dot = sum(a * b for a, b in zip(target_res, candidate_res))
    denom = math.sqrt(sum(a * a for a in target_res) * sum(b * b for b in candidate_res))
    corr = dot / denom if denom else math.nan
    best_lag, best_corr = 0, -2.0
    # This is a phase diagnostic only. It never changes the fixed-latency
    # score above; a non-zero best lag identifies a periodic-shape mismatch.
    for lag in range(-8, 9):
        left = target_res[max(0, lag):len(target_res) + min(0, lag)]
        right = candidate_res[max(0, -lag):len(candidate_res) - max(0, lag)]
        if len(left) < 16:
            continue
        lag_dot = sum(a * b for a, b in zip(left, right))
        lag_denom = math.sqrt(sum(a * a for a in left) * sum(b * b for b in right))
        lag_corr = lag_dot / lag_denom if lag_denom else -2.0
        if lag_corr > best_corr:
            best_lag, best_corr = lag, lag_corr
    print("frames=%d fixed_latency_samples=%.0f" % (len(aligned), args.latency))
    print("error_rmse_ct=%.6f error_mae_ct=%.6f correlation=%.6f" %
          (rms(errors), sum(abs(x) for x in errors) / len(errors), corr))
    print("phase_diagnostic_best_lag_frames=%d best_correlation=%.6f" % (best_lag, best_corr))
    print("up_rms_ct target=%.6f candidate=%.6f delta=%.6f" %
          (target_up, candidate_up, candidate_up - target_up))
    print("down_rms_ct target=%.6f candidate=%.6f delta=%.6f" %
          (target_down, candidate_down, candidate_down - target_down))


if __name__ == "__main__":
    main()
