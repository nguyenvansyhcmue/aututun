#!/usr/bin/env python3
"""Render a bounded causal rapid-return excursion parameter sweep.

This is an offline diagnostic tool. It does not alter plugin defaults, infer
Artist targets, or combine isolated control WAVs. Every render uses the one
verified hard-lock raw/Artist pair and reports phase-insensitive local quality
alongside the established full-WAV diagnostics.
"""

from __future__ import annotations

import argparse
import csv
import math
import re
import subprocess
from pathlib import Path


SAMPLE_RATE = 48_000.0
ALIGNMENT_SAMPLES = 2048
LOCAL_START_SECONDS = 4.7
LOCAL_END_SECONDS = 5.3

# Each threshold is taken from observed coherent-preview values rather than
# from a note name or a fixture timestamp. The sweep varies one mechanism:
# how much causal reversal evidence is needed before the existing pulse starts.
EVIDENCE_SETTINGS = ((2, 6.0), (3, 8.0), (4, 11.0), (5, 14.0), (6, 16.0))
HOLD_MILLISECONDS = (40.0, 50.0, 60.0)


def run(command: list[str], stdout_path: Path | None = None) -> str:
    result = subprocess.run(command, check=True, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if stdout_path is not None:
        stdout_path.write_text(result.stdout, encoding="utf-8")
    return result.stdout


def metric_map(path: Path) -> dict[str, float]:
    with path.open(newline="", encoding="utf-8") as stream:
        return {row["metric"]: float(row["value"])
                for row in csv.DictReader(stream)
                if row["metric"] != "window"}


def local_frame_metrics(path: Path) -> dict[str, float]:
    rows: list[dict[str, float]] = []
    with path.open(newline="", encoding="utf-8") as stream:
        for row in csv.DictReader(stream):
            time_seconds = float(row["timeMs"]) * 0.001
            if LOCAL_START_SECONDS <= time_seconds <= LOCAL_END_SECONDS:
                rows.append({key: float(value) for key, value in row.items()})
    if not rows:
        raise ValueError(f"no local target-match frames in {path}")

    def rms(values: list[float]) -> float:
        return math.sqrt(sum(value * value for value in values) / len(values))

    return {
        "local_waveform_frame_rms": rms([row["waveformNrmse"] for row in rows]),
        "local_envelope_rmse_db": rms([row["candidateRmsDb"] - row["targetRmsDb"] for row in rows]),
        "local_spectral_rmse_db": rms([row["spectralRmseDb"] for row in rows]),
        "local_spectral_tilt_rmse_db": rms([
            row["band6000_12000DbDiff"] - row["band80_400DbDiff"] for row in rows
        ]),
    }


def parse_reference_metrics(text: str) -> dict[str, float]:
    values = {key: float(value) for key, value in re.findall(r"(\w+)=([-+0-9.eE]+)", text)}
    return {
        "full_waveform_nrmse": values["waveform_nrmse"],
        "full_envelope_rmse_db": values["envelope_rmse_db"],
        "full_spectral_rmse_db": values["spectral_rmse_db"],
        "full_spectral_tilt_rmse_db": values["spectral_tilt_rmse_db"],
        "candidate_max_step": values["candidate_max_step"],
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output_directory", type=Path)
    parser.add_argument("--repo-root", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--coherent-frames", type=int, choices=[setting[0] for setting in EVIDENCE_SETTINGS],
                        help="render one evidence setting (all holds) to keep each batch short")
    parser.add_argument("--summarize-only", action="store_true",
                        help="read completed cases and write summary.csv without rendering")
    args = parser.parse_args()
    root = args.repo_root.resolve()
    output_root = args.output_directory.resolve()
    output_root.mkdir(parents=True, exist_ok=True)

    renderer = root / "build/VibeAutotuneSignalChainDiagnostic"
    matcher = root / "build/VibeAutotuneTargetReferenceMatch"
    contour = root / "build/VibeAutotuneIndependentPitchContour"
    breakdown = root / "scripts/measure_pitch_error_breakdown.py"
    raw = root / "renders/hardlock-pulse-anchor/hook2_raw_segment.wav"
    target = root / "renders/target-autotune-artist.wav"
    surface = root / "benchmark/autotune-artist-controls/natural-surface-phasefit-amplitude-candidate.csv"
    required = (renderer, matcher, contour, breakdown, raw, target, surface)
    missing = [str(path) for path in required if not path.exists()]
    if missing:
        parser.error("missing required paths: " + ", ".join(missing))

    summary_rows: list[dict[str, float | int | str]] = []
    settings = [setting for setting in EVIDENCE_SETTINGS
                if args.coherent_frames is None or setting[0] == args.coherent_frames]
    for coherent_frames, travel_cents in settings:
        for hold_milliseconds in HOLD_MILLISECONDS:
            case_name = f"frames{coherent_frames}_travel{travel_cents:g}_hold{hold_milliseconds:g}ms"
            case = output_root / case_name
            case.mkdir(exist_ok=True)
            if not args.summarize_only:
                diagnostic = [
                    str(renderer), str(raw), str(case),
                    "0", "0", "0", "0", "0", "0", "0", "0", "0", "0", "0",
                    str(surface), "1.0", "0.25", "-0.15", "0.666",
                    "0", "1", "0", str(coherent_frames), f"{travel_cents:g}", f"{hold_milliseconds:g}",
                ]
                run(diagnostic, case / "diagnostic.log")
                reference = run([
                    str(matcher), str(target), str(case / "full_default_render.wav"),
                    str(ALIGNMENT_SAMPLES), str(case / "reference_frames.csv"),
                ], case / "reference_metrics.txt")
                run([str(contour), str(target), "94", "600"], case / "target_contour.csv")
                run([str(contour), str(case / "full_default_render.wav"), "94", "600"], case / "candidate_contour.csv")
                run([
                    "python3", str(breakdown), str(case / "target_contour.csv"),
                    str(case / "candidate_contour.csv"), str(case / "pitch_audit"),
                    "--candidate-latency-samples", str(ALIGNMENT_SAMPLES),
                    "--start-seconds", str(LOCAL_START_SECONDS),
                    "--end-seconds", str(LOCAL_END_SECONDS),
                ])
            reference = (case / "reference_metrics.txt").read_text(encoding="utf-8")
            values: dict[str, float | int | str] = {
                "case": case_name,
                "minimum_coherent_frames": coherent_frames,
                "minimum_travel_cents": travel_cents,
                "hold_milliseconds": hold_milliseconds,
            }
            values.update(metric_map(case / "pitch_audit/summary.csv"))
            values.update(local_frame_metrics(case / "reference_frames.csv"))
            values.update(parse_reference_metrics(reference))
            summary_rows.append(values)
            print(case_name)

    # Rank only for triage: hard acceptance remains a manual evidence review.
    for row in summary_rows:
        row["triage_score"] = (
            float(row["f0_rmse_cents_jointly_voiced"])
            + 0.10 * float(row["absolute_error_p90_cents"])
            + 2.0 * float(row["local_spectral_rmse_db"])
            + 1.0 * float(row["local_envelope_rmse_db"])
        )
    summary_rows.sort(key=lambda row: float(row["triage_score"]))
    summary_path = output_root / "summary.csv"
    with summary_path.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(summary_rows[0]))
        writer.writeheader()
        writer.writerows(summary_rows)
    print(f"wrote {summary_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
