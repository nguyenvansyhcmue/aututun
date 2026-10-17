#!/usr/bin/env python3
"""Measure audio similarity while separating bounded periodic phase from artifacts.

The candidate is first aligned by the known pipeline latency.  Each voiced
analysis frame may then choose a small, causal-neutral analysis lag solely for
measurement.  This does not change audio, timing, or F0 evaluation; it keeps
an intentional pitch correction from being rejected only because its waveform
phase accumulates differently after the corrected event.
"""

from __future__ import annotations

import argparse
import csv
import math
import struct
import wave
from pathlib import Path

import numpy as np


FRAME_SAMPLES = 2048
HOP_SAMPLES = 512
SILENCE_FLOOR_DB = -55.0


def read_mono_pcm(path: Path) -> tuple[int, list[float]]:
    with wave.open(str(path), "rb") as stream:
        channels = stream.getnchannels()
        width = stream.getsampwidth()
        sample_rate = stream.getframerate()
        frame_count = stream.getnframes()
        compression = stream.getcomptype()
        payload = stream.readframes(frame_count)

    if compression != "NONE" or channels < 1 or sample_rate <= 0:
        raise ValueError(f"{path} must be an uncompressed PCM WAV")
    if width not in (2, 3, 4):
        raise ValueError(f"{path} has unsupported PCM width: {width * 8} bits")

    sample_count = frame_count * channels
    if width == 2:
        integers = struct.unpack(f"<{sample_count}h", payload)
        scale = float(1 << 15)
    elif width == 4:
        integers = struct.unpack(f"<{sample_count}i", payload)
        scale = float(1 << 31)
    else:
        integers = []
        for offset in range(0, len(payload), 3):
            unsigned = payload[offset] | (payload[offset + 1] << 8) | (payload[offset + 2] << 16)
            integers.append(unsigned - (1 << 24) if unsigned & (1 << 23) else unsigned)
        scale = float(1 << 23)

    mono: list[float] = []
    for frame in range(frame_count):
        first = frame * channels
        mono.append(sum(integers[first:first + channels]) / (channels * scale))
    return sample_rate, mono


def rms_db(samples: list[float] | np.ndarray) -> float:
    energy = float(np.mean(np.square(samples))) if len(samples) else 0.0
    return 20.0 * math.log10(math.sqrt(energy) + 1.0e-12)


def best_phase_match(reference: np.ndarray, candidate_search: np.ndarray) -> tuple[int, float, np.ndarray]:
    """Return the best normalized-correlation frame from a bounded lag search."""
    windows = np.lib.stride_tricks.sliding_window_view(candidate_search, len(reference))
    centered_reference = reference - np.mean(reference)
    centered_windows = windows - np.mean(windows, axis=1, keepdims=True)
    denominator = np.sqrt(np.sum(np.square(centered_windows), axis=1)
                          * np.sum(np.square(centered_reference)))
    correlations = np.divide(centered_windows @ centered_reference, denominator,
                             out=np.zeros_like(denominator), where=denominator > 1.0e-20)
    index = int(np.argmax(correlations))
    return index, float(correlations[index]), windows[index]


def percentile(values: list[float], fraction: float) -> float:
    if not values:
        return math.nan
    ordered = sorted(values)
    return ordered[round((len(ordered) - 1) * fraction)]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("reference_wav", type=Path)
    parser.add_argument("candidate_wav", type=Path)
    parser.add_argument("output_directory", type=Path)
    parser.add_argument("--candidate-alignment-samples", type=int, default=2048)
    parser.add_argument("--maximum-phase-lag-samples", type=int, default=128,
                        help="symmetric per-frame search radius; default spans roughly half a vocal period")
    parser.add_argument("--start-seconds", type=float, default=None)
    parser.add_argument("--end-seconds", type=float, default=None)
    args = parser.parse_args()

    if args.maximum_phase_lag_samples < 0:
        parser.error("--maximum-phase-lag-samples must be non-negative")
    if args.start_seconds is not None and args.end_seconds is not None and args.end_seconds < args.start_seconds:
        parser.error("--end-seconds must not precede --start-seconds")

    sample_rate, reference_values = read_mono_pcm(args.reference_wav)
    candidate_rate, candidate_values = read_mono_pcm(args.candidate_wav)
    if sample_rate != candidate_rate:
        parser.error("reference and candidate sample rates must match")

    reference = np.asarray(reference_values, dtype=np.float64)
    candidate = np.asarray(candidate_values, dtype=np.float64)
    start_sample = 0 if args.start_seconds is None else max(0, round(args.start_seconds * sample_rate))
    final_reference_sample = len(reference) if args.end_seconds is None else min(len(reference), round(args.end_seconds * sample_rate))
    frame_rows: list[dict[str, float | int]] = []
    phase_relaxed_error = 0.0
    reference_energy = 0.0

    for start in range(start_sample, final_reference_sample - FRAME_SAMPLES + 1, HOP_SAMPLES):
        fixed_candidate_start = start + args.candidate_alignment_samples
        if fixed_candidate_start - args.maximum_phase_lag_samples < 0:
            continue
        if fixed_candidate_start + FRAME_SAMPLES + args.maximum_phase_lag_samples > len(candidate):
            continue
        reference_frame = reference[start:start + FRAME_SAMPLES]
        if rms_db(reference_frame) < SILENCE_FLOOR_DB:
            continue

        search_start = fixed_candidate_start - args.maximum_phase_lag_samples
        search_end = fixed_candidate_start + FRAME_SAMPLES + args.maximum_phase_lag_samples
        best_index, best_correlation, best_candidate_frame = best_phase_match(
            reference_frame, candidate[search_start:search_end])
        best_lag = best_index - args.maximum_phase_lag_samples
        error = float(np.sum(np.square(reference_frame - best_candidate_frame)))
        energy = float(np.sum(np.square(reference_frame)))
        phase_relaxed_error += error
        reference_energy += energy
        frame_rows.append({
            "time_ms": 1000.0 * start / sample_rate,
            "best_phase_lag_samples": best_lag,
            "phase_coherence": best_correlation,
            "phase_relaxed_nrmse": math.sqrt(error / max(energy, 1.0e-20)),
            "reference_rms_db": rms_db(reference_frame),
            "candidate_rms_db": rms_db(best_candidate_frame),
        })

    if not frame_rows:
        parser.error("no non-silent complete frames in the requested window")

    args.output_directory.mkdir(parents=True, exist_ok=True)
    with (args.output_directory / "phase_relaxed_frames.csv").open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(frame_rows[0]))
        writer.writeheader()
        writer.writerows(frame_rows)

    coherences = [float(row["phase_coherence"]) for row in frame_rows]
    lags = [abs(float(row["best_phase_lag_samples"])) for row in frame_rows]
    summary = [
        ("sample_rate", sample_rate),
        ("candidate_alignment_samples", args.candidate_alignment_samples),
        ("maximum_phase_lag_samples", args.maximum_phase_lag_samples),
        ("frames", len(frame_rows)),
        ("phase_relaxed_nrmse", math.sqrt(phase_relaxed_error / max(reference_energy, 1.0e-20))),
        ("phase_coherence_p50", percentile(coherences, 0.50)),
        ("phase_coherence_p10", percentile(coherences, 0.10)),
        ("absolute_phase_lag_samples_p50", percentile(lags, 0.50)),
        ("absolute_phase_lag_samples_p90", percentile(lags, 0.90)),
    ]
    with (args.output_directory / "summary.csv").open("w", newline="", encoding="utf-8") as stream:
        writer = csv.writer(stream)
        writer.writerow(["metric", "value"])
        writer.writerows(summary)
    print(f"wrote {args.output_directory / 'phase_relaxed_frames.csv'}")
    print(f"wrote {args.output_directory / 'summary.csv'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
