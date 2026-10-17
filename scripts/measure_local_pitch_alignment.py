#!/usr/bin/env python3
"""Report the local F0 alignment between Artist and a Vibe render.

Usage:
  scripts/measure_local_pitch_alignment.py TARGET_CONTOUR CANDIDATE_CONTOUR OUTPUT_CSV

The candidate contour is kept on its native output timeline.  Every 0.5-s
window scans latency offsets in 256-sample increments; this reveals a
time-varying response that a single whole-file 2048-sample alignment hides.
"""

import csv
import math
import sys

SAMPLE_RATE = 48000
WINDOW_SAMPLES = SAMPLE_RATE // 2
MIN_OFFSET = 1024
MAX_OFFSET = 4096
OFFSET_STEP = 256


def read_contour(path):
    result = {}
    with open(path, newline="") as stream:
        for row in csv.reader(stream):
            if not row or row[0] == "sample":
                continue
            frequency = float(row[1])
            if frequency > 0.0:
                result[int(row[0])] = 1200.0 * math.log2(frequency / 440.0)
    return result


def rms_for_offset(target, candidate, first, last, offset):
    squared_error = 0.0
    count = 0
    for sample, target_cents in target.items():
        if first <= sample < last and sample + offset in candidate:
            error = target_cents - candidate[sample + offset]
            squared_error += error * error
            count += 1
    return (math.sqrt(squared_error / count), count) if count else (float("inf"), 0)


def main():
    if len(sys.argv) != 4:
        raise SystemExit("usage: measure_local_pitch_alignment.py TARGET_CONTOUR CANDIDATE_CONTOUR OUTPUT_CSV")
    target = read_contour(sys.argv[1])
    candidate = read_contour(sys.argv[2])
    last_sample = min(max(target, default=0), max(candidate, default=0) - MIN_OFFSET)
    with open(sys.argv[3], "w", newline="") as stream:
        writer = csv.writer(stream)
        writer.writerow(("window_start_sample", "window_start_seconds", "best_offset_samples", "f0_rmse_cents", "matched_frames"))
        for first in range(0, last_sample + 1, WINDOW_SAMPLES):
            last = first + WINDOW_SAMPLES
            best = min(((*rms_for_offset(target, candidate, first, last, offset), offset)
                        for offset in range(MIN_OFFSET, MAX_OFFSET + 1, OFFSET_STEP)),
                       key=lambda item: item[0])
            error, count, offset = best
            if count:
                writer.writerow((first, f"{first / SAMPLE_RATE:.3f}", offset, f"{error:.6f}", count))


if __name__ == "__main__":
    main()
