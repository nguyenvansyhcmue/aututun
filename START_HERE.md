# VibeAutotune — start here on a new machine

This is the operating brief for the next Codex session. It supersedes any conflicting interpretation in older notes. Read this file completely before editing code, building an AU, or installing anything into Logic.

## Ready-to-use prompt

Open this repository as the workspace, then send Codex this exact prompt:

```text
Read START_HERE.md completely and follow it as the active handoff. First inspect git status and verify the target manifest. Continue VibeAutotune calibration without changing or installing the AU until a measured candidate passes the stated local and whole-file acceptance checks. Do not repeat experiments listed as rejected.
```

## Source of truth

| Need | File / location |
| --- | --- |
| Control-target mapping | `benchmark/autotune-artist-controls/TARGET_MANIFEST.csv` |
| Control stimuli and measurement rules | `benchmark/autotune-artist-controls/README.md` |
| Rapid-legato technical history | `CODEX_HANDOFF_2026-09-28.md` |
| Portable vocal comparison bundle | `data/session-2026-09-28/README.md` |
| Artist vocal reference | `renders/target-autotune-artist.wav` |
| Matching raw vocal | `renders/hardlock-pulse-anchor/hook2_raw_segment.wav` |
| Listening-approved AU binary | `releases/VibeAutotune-1.0.3-stable.component` |
| Main correction code | `VibeAutotune/CorrectionTrajectory.h` |
| TD-PSOLA renderer | `VibeAutotune/CausalTDPSOLARenderer.h` |

The listening-approved `1.0.3` component is a binary reference only. Its exact source was not preserved; never claim that the present source tree is bit-identical to it.

## Target-data rules

`TARGET_MANIFEST.csv` is authoritative. Select a target by its `id`, not by its legacy filename. It includes the full known configuration, SHA-256 and a status for each render.

- Use only `verified` targets for normal single-control calibration.
- `verified_noncanonical_retune` Flex targets are valid at Retune 10 ms, but do not stand in for the still-missing Retune 25 ms data.
- Natural Vibrato `0 ms` and `+9 ms` are explicitly missing. Do not invent a curve through either hole.
- Never use files in `archive-controls-on-flex-input/` to fit Humanize or Natural Vibrato independently: their input/control state is mixed.
- No complete four-knob interaction target matrix exists. A one-control fit must not be presented as proof of arbitrary combined-control equivalence.

## Fixed vocal-reference preset

For the main 11-second Artist comparison:

```text
Input type: Alto
Key / scale: F# Natural Minor
Tracking: 50
Retune Speed: 0 ms
Flex-Tune: 0
Humanize: 0
Natural Vibrato: 0
Classic / Formant: off
Candidate alignment: 2048 samples at 48 kHz
```

The audible failure to preserve is the rapid A -> B -> A phrase at **4.7–5.3 seconds**. The candidate can miss the final returned F# and hold the prior note. Whole-file scores cannot approve a change by themselves.

## Acceptance requirements

Before calling an audio/control change accepted, run the relevant tests and compare it with the exact mapped Artist target. A candidate must satisfy all of these:

1. Improve or preserve the local F0 contour in the relevant phrase/control window, particularly 4.7–5.3 s for rapid legato.
2. Improve or preserve whole-file waveform, envelope and spectral metrics.
3. Not increase max sample discontinuity or create audible clicks, detached harmonics, parallel-voice layers, or note-skipping.
4. Pass `VibeAutotuneCorrectionTrajectoryTest` and `VibeAutotuneCausalTDPSOLATest`.
5. Keep an output WAV, measurement CSV and command record alongside every accepted or rejected experiment.

For the vocal reference, baseline metrics and accepted/rejected output are already in `data/session-2026-09-28/measurements/`.

## Known rejected directions

Do not repeat these without a new measured hypothesis:

- Phase-continuous / dual-renderer paths: they exposed detached harmony-like layers.
- Only lowering target-selection thresholds or only extending a return transition: can make the missing return appear but produces grain-scale 10–36-cent steps and clicks.
- Approving from global NRMSE alone.
- Changing saturation, gate, colour or Natural Vibrato while diagnosing the rapid-legato correction trajectory.

The safe design direction is continuous correction ratio at TD-PSOLA synthesis marks, while separately ensuring future OLA grains cannot preserve a stale note long enough to sound as a second voice. Do not reset the renderer or crossfade two renderers as a shortcut.

## First commands

```zsh
git status --short
git log -1 --oneline
git diff --check

cmake -S . -B build
cmake --build build --target \
  VibeAutotuneCorrectionTrajectoryTest \
  VibeAutotuneCausalTDPSOLATest \
  VibeAutotuneSignalChainDiagnostic \
  VibeAutotuneTargetReferenceMatch -j 6

build/VibeAutotuneCorrectionTrajectoryTest
build/VibeAutotuneCausalTDPSOLATest
```

Do not run a blanket reset or checkout in a dirty worktree. Inspect each change before keeping or reverting it.

## Render and compare the vocal baseline

```zsh
mkdir -p renders/current-measured
build/VibeAutotuneSignalChainDiagnostic \
  renders/hardlock-pulse-anchor/hook2_raw_segment.wav \
  renders/current-measured \
  0 1 0 0 0 0 0

build/VibeAutotuneTargetReferenceMatch \
  renders/target-autotune-artist.wav \
  renders/current-measured/full_default_render.wav \
  2048 \
  renders/current-measured/reference_frames.csv
```

Only install an AU after a candidate is accepted. Installation into the user Audio Units folder needs no `sudo`:

```zsh
ditto "build/VibeAutotune_artefacts/Release/AU/VibeAutotune.component" \
  "$HOME/Library/Audio/Plug-Ins/Components/VibeAutotune.component"
```

Then use Logic Plug-in Manager → VibeAutotune → **Reset & Rescan Selection** and open a fresh instance.
