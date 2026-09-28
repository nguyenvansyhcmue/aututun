# VibeAutotune — Handoff, 2026-09-28

## Purpose

Continue matching VibeAutotune to the Auto-Tune Artist reference without
repeating rejected experiments. The immediate audible defect is rapid
legato / reversal: a note in an A -> B -> A phrase is missing or merges into
a glide. The most visible instance is around 4.7–5.3 seconds.

This document records facts measured in this working session. It does **not**
claim to describe Antares' proprietary implementation; conclusions about its
internal architecture are inferences from its audio output.

## Authoritative files

| Role | Path |
| --- | --- |
| Artist reference output | `renders/target-autotune-artist.wav` |
| Matching raw benchmark segment | `renders/hardlock-pulse-anchor/hook2_raw_segment.wav` |
| Larger original raw vocal | `/Users/hiimdrdey/Downloads/hook 2.wav` |
| Benchmark notes | `benchmark/reference-fsharp-minor-alto/README.md` |
| Main trajectory/control code | `VibeAutotune/CorrectionTrajectory.h` |
| TD-PSOLA renderer | `VibeAutotune/CausalTDPSOLARenderer.h` |
| Offline render/telemetry tool | `tests/SignalChainDiagnostic.cpp` |
| WAV reference metric tool | `tests/TargetReferenceMatch.cpp` |
| Independent F0 export | `tests/IndependentPitchContour.cpp` |

The reference comparison preset is exactly: **Alto, F# Natural Minor,
Tracking 50, Retune 0 ms, Flex-Tune 0, Humanize 0, Natural Vibrato 0**.
The production processor latency is 2048 samples at 48 kHz (42.67 ms).
When comparing a direct render against the reference, pass candidate alignment
`2048` to `VibeAutotuneTargetReferenceMatch`.

## Current source / build state

- Source is restored to the `1.0.5` behavior for the rapid-return trial.
  Do not treat a cached `1.0.6` artefact as the source of truth.
- The current AU version configured in both CMake files is **1.0.5**.
- `VibeAutotuneCorrectionTrajectoryTest` passes.
- `VibeAutotuneCausalTDPSOLATest` passes (`TD_PSOLA_RENDERER: PASS`).
- `git diff --check` passes.
- The worktree is intentionally dirty from earlier ongoing work. Do **not**
  run `git reset`, `git checkout`, or blanket reverts. Inspect individual
  changes before editing.
- The listening-approved AU 1.0.3 is preserved at
  `releases/VibeAutotune-1.0.3-stable.component`. Its exact C++ worktree was
  never committed (Git has source history only through 1.0.2), so this binary
  is the golden reference for cross-machine listening and comparison.

The production renderer is the normal Causal TD-PSOLA path. Do not re-enable
the phase-continuous renderer experiment: listening tests exposed pronounced
parallel/harmony-like layers.

## Measurements made this session

### Current 1.0.5 render

Produced with:

```zsh
mkdir -p renders/current-1.0.5-measured
build/VibeAutotuneSignalChainDiagnostic \
  renders/hardlock-pulse-anchor/hook2_raw_segment.wav \
  renders/current-1.0.5-measured \
  0 1 0 0 0 0 0

build/VibeAutotuneTargetReferenceMatch \
  renders/target-autotune-artist.wav \
  renders/current-1.0.5-measured/full_default_render.wav \
  2048 \
  renders/current-1.0.5-measured/reference_frames.csv
```

Whole-file metrics:

| Metric | Current 1.0.5 |
| --- | ---: |
| Waveform NRMSE | 1.32926 |
| Envelope RMSE | 1.88324 dB |
| Spectral RMSE | 3.29885 dB |
| Spectral tilt RMSE | 4.37206 dB |
| Candidate max sample step | 0.147347 |
| Reference max sample step | 0.156891 |

These global values alone must not be used to approve a change: they can
improve while the audible A -> B -> A note is still missing.

### 4.7–5.3 s rapid phrase

Independent YIN contours were exported from target and candidate. Candidate
timestamps were shifted back by 2048 samples before comparison.

| Observation | Reference Artist | Vibe 1.0.5 |
| --- | --- | --- |
| Sequence | ~208 Hz -> 220 Hz -> ~208 Hz -> 220 Hz | ~208 Hz -> 220 Hz, then remains near 220 Hz |
| Peak | 220.18 Hz at 4826.67 ms | 222.21 Hz at 4784.00 ms |
| F0 RMSE, 4.7–5.3 s | baseline | 33.12 cents |

At about 5.05–5.10 s the Artist output clearly reaches the returned F#
(~208 Hz). The Vibe output stays near 220 Hz through 5.3 s.

`renders/current-1.0.5-measured/full_default_events.csv` confirms the
problem: after the commit to MIDI 57 at 4922.67 ms, there is no later commit
to MIDI 56 in this phrase. The source F0 descends close to the F#/A midpoint
but the present discrete target selector never publishes MIDI 56 early enough
for the delayed output to contain that note.

## Experiments in this session that were rejected

1. **Remove only the 48 ms immediate-return guard.**
   No output change for this case. The selector had not produced the return
   candidate, so removing the guard could not create an event.

2. **Remove the `trajectoryPreviewDirection` reverse-direction veto.**
   Also did not create the missing target commit for this input. It is not the
   root cause of this measured failure.

3. **Enable existing `enableHardTuneTrajectoryPreview`.**
   It improved global waveform NRMSE to 1.24910, but the key local F0 error
   only changed from 33.12 to 32.50 cents and still emitted no MIDI-56 commit
   in the missing-note region. This is not accepted and is restored to
   `false`.

Do not reinstall or publish any of those trials as a solution.

4. **Lower only the renderer-crossing return gate (700 -> 500 ct/s).**
   No meaningful result: the preview-travel gate still blocked the phrase.

5. **Lower returned-note preview travel (20 -> 14 cents), lower motion
   (700 -> 500 ct/s), and lengthen the linear crossing (12 -> 48 ms).**
   Local F0 RMSE improved from 33.12 to 21.29 cents, but the renderer emitted
   18 steps above 10 cents (peak 36 cents) in the affected region. Smoothstep
   did not remove the grain-level steps. Both audio changes were rejected and
   restored; only read-only selector-preview telemetry was retained in the
   diagnostic CSV.

## Important architectural conclusion

The prior engine is too event-based for this phrase:

```text
detect discrete target note -> commit target -> build transition -> renderer grain handoff
```

Adding guards, persistence counters, two-frame confirmations, or special
A->B->A plateaus can suppress either tracker chatter **or real short notes**.
They have repeatedly traded a glide for a missing note.

The required design direction is instead:

```text
F0 tracker -> legal-scale attractor -> continuous correction trajectory -> TD-PSOLA ratio per synthesis mark
```

`currentTargetNote` may remain as musical metadata/telemetry, but it must not
be the sole state that determines whether the renderer is allowed to move.
The renderer should receive a continuous `correctionCents` / pitch ratio. A
hard tune is a very fast trajectory, not a resettable series of renderer-note
events.

## Next implementation plan

Work in this order. Do not edit saturation, gate, voiced colour, natural
vibrato, or phase-continuous rendering while solving this control defect.

1. **Map the existing control path.** Trace where `desiredCorrectionCents`,
   `retuneCorrectionCents`, and `requestedRendererRatio` are assigned in
   `CorrectionTrajectory.h`. Establish one explicit continuous state variable
   for the correction sent to TD-PSOLA.

2. **Separate scale attraction from note commits.** For each valid F0 frame,
   compute the nearest legal F#-minor attraction point and its cents error.
   Preserve a continuous correction state between frames. Do not reset it
   when `currentTargetNote` changes.

3. **Make Retune Speed the only main time law.** Apply a documented one-pole
   / exponential time constant to correction cents. At 0 ms, use an
   intentionally small de-zipper only if necessary for click-free synthesis;
   at slow retune values, do not let correction fail to reach the note before
   the next frame.

4. **Keep short-note evidence independent of synthesis ownership.** If a
   short F# needs early recognition, model this as a direction-confirmed,
   continuous attraction strength—not as a permanent hard note commit nor a
   grain replacement event.

5. **Only after control contour matches, audit TD-PSOLA.** Each synthesis
   mark must use the current continuous ratio. Existing future OLA grains may
   finish their audible overlap; do not reset the renderer or run two
   renderers in parallel. If an audible old grain causes a real duplicate
   pitch, implement event-owned future grain replacement separately and
   measure it against the control contour first.

6. **Require both local and whole-file acceptance.** At minimum compare:
   - F0 RMSE in 4.7–5.3 s;
   - target commit/continuous-ratio telemetry around 4.9–5.2 s;
   - waveform/spectral metrics over the full 11 s;
   - max sample step and listening for click/harmony layers.

Never accept a change solely because whole-file NRMSE improves.

## Useful commands

Build diagnostics and tests:

```zsh
cmake -S . -B build
cmake --build build --target \
  VibeAutotune_AU \
  VibeAutotuneSignalChainDiagnostic \
  VibeAutotuneTargetReferenceMatch \
  VibeAutotuneIndependentPitchContour \
  VibeAutotuneCorrectionTrajectoryTest \
  VibeAutotuneCausalTDPSOLATest -j 6

build/VibeAutotuneCorrectionTrajectoryTest
build/VibeAutotuneCausalTDPSOLATest
```

Export F0 contours:

```zsh
build/VibeAutotuneIndependentPitchContour \
  renders/target-autotune-artist.wav 94 600 > /private/tmp/target_contour.csv

build/VibeAutotuneIndependentPitchContour \
  renders/current-1.0.5-measured/full_default_render.wav 94 600 \
  > /private/tmp/candidate_contour.csv
```

When installing a deliberately accepted AU in Logic, use `ditto`; no `sudo`
is required:

```zsh
ditto "/Users/hiimdrdey/autotune/build/VibeAutotune_artefacts/Release/AU/VibeAutotune.component" \
      "$HOME/Library/Audio/Plug-Ins/Components/VibeAutotune.component"
```

Then use Logic Plug-in Manager -> VibeAutotune -> **Reset & Rescan Selection**
and instantiate a fresh plug-in instance.
