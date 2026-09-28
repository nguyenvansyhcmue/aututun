# Portable comparison dataset — 2026-09-28

This directory is a self-contained handoff dataset for continuing VibeAutotune
work on another computer. It contains the audio fixtures, renders, pitch
contours, waveform metrics, target/OLA event telemetry, and the latest
accepted/rejected experiment state.

## Fixed comparison preset

| Setting | Value |
| --- | --- |
| Input type | Alto |
| Key / scale | F# Natural Minor |
| Tracking | 50 |
| Retune Speed | 0 ms |
| Flex-Tune | 0 |
| Humanize | 0 |
| Natural Vibrato | 0 |
| Candidate alignment | 2048 samples at 48 kHz |

## Audio

| File | Meaning |
| --- | --- |
| `audio/raw-input.wav` | Mono vocal source used for every render below. |
| `audio/artist-target.wav` | Auto-Tune Artist reference output. This is the target. |
| `audio/vibe-baseline-1.0.5.wav` | Accepted baseline sound before the rejected return-contour trials. |
| `audio/vibe-return-gated-48ms-rejected.wav` | Rejected experiment. It improves F0 tracking during the short return but creates grain-level steps/click risk. Never use as production baseline. |

## Measurements

### `measurements/baseline-1.0.5/`

Current stable audio/control baseline. `full_default_render.wav` is identical
to the baseline audio export. It includes:

- `reference_metrics.txt`, `reference_frames.csv`: target waveform/spectral
  comparison.
- `candidate_contour.csv`, plus top-level `target_contour.csv`: independent
  YIN F0 values at a 256-sample hop.
- `full_default_intervals.csv`: control, selector, renderer and OLA telemetry.
- `full_default_events.csv`: target commits, grain spawns, renderer steps and
  bridges.
- `plateau_audit.csv`: source-control-to-rendered-pitch audit.
- `case_metrics.csv`, `diagnostic.log`: reproducibility record.

Baseline reference metrics:

```text
waveform_nrmse=1.32926
envelope_rmse_db=1.88324
spectral_rmse_db=3.29885
spectral_tilt_rmse_db=4.37206
candidate_max_step=0.147347
```

The known defect remains: in the 4.7–5.3 s A -> B -> A phrase, the final F#
return is not sufficiently represented. Local F0 RMS error is 33.12 cents.

### `measurements/rejected-return-gated-48ms/`

The closest recent experiment to producing the missing return. It lowered a
direction-confirmed return gate and used a 48 ms predicted correction contour.

It is retained only for analysis:

```text
local F0 RMS error: 33.12 -> 21.29 cents
renderer steps >10 cents in 5.0–5.15 s: 18
largest renderer step: 36 cents
```

It was rejected because those synthesis steps are audible as click/roughness
or a detached harmonic layer. It is evidence that target timing is not enough:
the future TD-PSOLA OLA grain handoff must be corrected too.

## What to do next

1. Keep `audio/vibe-baseline-1.0.5.wav` as the listening baseline.
2. Compare every candidate against `audio/artist-target.wav`, with the same
   2048-sample candidate alignment.
3. Use the rejected trial to locate the precise handoff: around source/control
   5.04–5.09 s, its event CSV records the rapid ratio change and the 36-cent
   renderer steps.
4. Solve handoff at the future OLA-grain level; do not merely lengthen or
   lower target selector thresholds.
5. A candidate must improve the local F0 contour **and** not increase
   grain/sample discontinuities or make audible harmony layers.

## Re-run reference metrics

From the repository root:

```zsh
build/VibeAutotuneTargetReferenceMatch \
  data/session-2026-09-28/audio/artist-target.wav \
  data/session-2026-09-28/audio/vibe-baseline-1.0.5.wav \
  2048
```
