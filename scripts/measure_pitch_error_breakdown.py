#!/usr/bin/env python3
"""Break an aligned pair of independent-F0 contours into actionable errors.

The manifest benchmark reports one F0 RMS number.  This companion keeps the
same sample-domain alignment but exposes whether that RMS comes from ordinary
near-note error, gross note mistakes, or availability of voiced estimates.
It does not infer a plugin's internal target or modify audio.

The input CSVs must have the columns emitted by VibeAutotuneIndependentPitchContour:
``sample,independent_f0_hz``.  Candidate timestamps are shifted backwards by
the configured renderer latency before matching the reference grid.
"""

from __future__ import annotations

import argparse
import csv
import math
from pathlib import Path
from typing import Iterable


FINE_ERROR_CENTS = 50.0
OCTAVE_OR_LARGER_CENTS = 600.0


def parse_f0(value: str) -> float | None:
    try:
        frequency = float(value)
    except ValueError:
        return None
    return frequency if math.isfinite(frequency) and frequency > 0.0 else None


def read_contour(path: Path) -> dict[int, float | None]:
    with path.open(newline="", encoding="utf-8") as stream:
        reader = csv.DictReader(stream)
        required = {"sample", "independent_f0_hz"}
        if reader.fieldnames is None or not required.issubset(reader.fieldnames):
            raise ValueError(f"{path} must contain columns: sample,independent_f0_hz")

        contour: dict[int, float | None] = {}
        for row in reader:
            try:
                sample = int(row["sample"])
            except (TypeError, ValueError) as error:
                raise ValueError(f"invalid sample in {path}: {row.get('sample')!r}") from error
            contour[sample] = parse_f0(row["independent_f0_hz"])
    return contour


def percentile(values: list[float], fraction: float) -> float:
    if not values:
        return math.nan
    ordered = sorted(values)
    index = round((len(ordered) - 1) * fraction)
    return ordered[index]


def error_class(absolute_cents: float) -> str:
    if absolute_cents < FINE_ERROR_CENTS:
        return "fine_under_50ct"
    if absolute_cents < OCTAVE_OR_LARGER_CENTS:
        return "note_range_50_to_600ct"
    # This is intentionally conservative: >=600 ct is octave-scale or
    # larger, but can also represent another gross target/analysis mistake.
    # The label never claims that every such frame is an octave error.
    return "octave_scale_or_larger"


def write_summary(path: Path, rows: Iterable[tuple[str, str]]) -> None:
    with path.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.writer(stream)
        writer.writerow(["metric", "value"])
        writer.writerows(rows)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("reference_contour", type=Path)
    parser.add_argument("candidate_contour", type=Path)
    parser.add_argument("output_directory", type=Path)
    parser.add_argument("--candidate-latency-samples", type=int, default=2048,
                        help="candidate render latency to shift backwards (default: 2048)")
    parser.add_argument("--sample-rate", type=float, default=48000.0,
                        help="CSV timeline sample rate for time_s output (default: 48000)")
    parser.add_argument("--start-seconds", type=float, default=None,
                        help="optional inclusive local-window start")
    parser.add_argument("--end-seconds", type=float, default=None,
                        help="optional inclusive local-window end")
    args = parser.parse_args()

    if args.sample_rate <= 0.0 or not math.isfinite(args.sample_rate):
        parser.error("--sample-rate must be finite and positive")
    if args.start_seconds is not None and args.end_seconds is not None and args.end_seconds < args.start_seconds:
        parser.error("--end-seconds must not precede --start-seconds")

    reference = read_contour(args.reference_contour)
    candidate_raw = read_contour(args.candidate_contour)
    candidate = {sample - args.candidate_latency_samples: f0 for sample, f0 in candidate_raw.items()}
    common_samples = sorted(set(reference).intersection(candidate))
    if args.start_seconds is not None:
        common_samples = [sample for sample in common_samples if sample / args.sample_rate >= args.start_seconds]
    if args.end_seconds is not None:
        common_samples = [sample for sample in common_samples if sample / args.sample_rate <= args.end_seconds]

    args.output_directory.mkdir(parents=True, exist_ok=True)
    frame_path = args.output_directory / "frame_errors.csv"
    matched_errors: list[float] = []
    class_counts = {"fine_under_50ct": 0, "note_range_50_to_600ct": 0, "octave_scale_or_larger": 0}
    reference_voiced = 0
    candidate_voiced = 0
    jointly_voiced = 0
    with frame_path.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.writer(stream)
        writer.writerow(["sample", "time_s", "reference_f0_hz", "candidate_f0_hz",
                         "cents_error", "absolute_cents_error", "classification"])
        for sample in common_samples:
            reference_f0 = reference[sample]
            candidate_f0 = candidate[sample]
            reference_voiced += reference_f0 is not None
            candidate_voiced += candidate_f0 is not None
            if reference_f0 is None or candidate_f0 is None:
                writer.writerow([sample, f"{sample / args.sample_rate:.9f}", reference_f0 or "nan",
                                 candidate_f0 or "nan", "nan", "nan", "unmatched_voicing"])
                continue

            cents = 1200.0 * math.log2(candidate_f0 / reference_f0)
            absolute_cents = abs(cents)
            classification = error_class(absolute_cents)
            matched_errors.append(cents)
            class_counts[classification] += 1
            jointly_voiced += 1
            writer.writerow([sample, f"{sample / args.sample_rate:.9f}", f"{reference_f0:.9f}",
                             f"{candidate_f0:.9f}", f"{cents:.6f}", f"{absolute_cents:.6f}", classification])

    absolute_errors = [abs(error) for error in matched_errors]
    rmse = math.sqrt(sum(error * error for error in matched_errors) / len(matched_errors)) if matched_errors else math.nan
    precision = jointly_voiced / candidate_voiced if candidate_voiced else math.nan
    recall = jointly_voiced / reference_voiced if reference_voiced else math.nan
    window = "full" if args.start_seconds is None else f"{args.start_seconds:g}s_to_{args.end_seconds:g}s"
    summary_rows = [
        ("window", window),
        ("candidate_latency_samples", str(args.candidate_latency_samples)),
        ("common_grid_frames", str(len(common_samples))),
        ("reference_voiced_frames", str(reference_voiced)),
        ("candidate_voiced_frames", str(candidate_voiced)),
        ("jointly_voiced_frames", str(jointly_voiced)),
        ("voicing_precision_on_common_grid", f"{precision:.9f}"),
        ("voicing_recall_on_common_grid", f"{recall:.9f}"),
        ("f0_rmse_cents_jointly_voiced", f"{rmse:.6f}"),
        ("absolute_error_p50_cents", f"{percentile(absolute_errors, 0.50):.6f}"),
        ("absolute_error_p90_cents", f"{percentile(absolute_errors, 0.90):.6f}"),
        ("absolute_error_p95_cents", f"{percentile(absolute_errors, 0.95):.6f}"),
        ("absolute_error_p99_cents", f"{percentile(absolute_errors, 0.99):.6f}"),
    ]
    for label, count in class_counts.items():
        share = count / jointly_voiced if jointly_voiced else math.nan
        summary_rows.extend(((f"{label}_frames", str(count)), (f"{label}_share", f"{share:.9f}")))

    summary_path = args.output_directory / "summary.csv"
    write_summary(summary_path, summary_rows)
    print(f"wrote {summary_path}")
    print(f"wrote {frame_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
