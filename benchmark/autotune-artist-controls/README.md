# Auto-Tune Artist control calibration

These files identify observable control curves; they do not attempt to copy
Antares implementation details. All inputs are mono, 48 kHz, 24-bit,
synthetic voiced vowels centred on F#4. Their target under F# Natural Minor
is therefore always F#4.

## Fixed Artist settings

For every bounce set: Alto/Tenor, key F#, Natural Minor, Retune 10 ms for
Humanize and Natural Vibrato, Tracking 50, Transpose 0, Detune 440 Hz,
Classic off, Formant off, and Throat at its neutral/default position. Do not
normalise, trim latency, dither, or put another plug-in after Artist.

For a Flex run, use the indicated Flex value while Humanize and Natural
Vibrato stay at zero. Create two Flex runs: Retune 0 ms and Retune 25 ms.
For a Humanize run, Flex and Natural Vibrato stay at zero. For a Natural
Vibrato run, Flex and Humanize stay at zero.

## Authoritative target data

`TARGET_MANIFEST.csv` is the source of truth for every existing Artist WAV.
It records the exact control values, source timeline, duration and SHA-256.
The original filenames remain untouched, including legacy names that mention
the wrong control.  Measurement tools must select a target by manifest `id`,
not by parsing the filename.

Only rows whose `status` is `verified` are valid calibration targets. The
timeline prefix in a legacy filename identifies the source timeline only; it
does not imply that Flex or Humanize was enabled. The control values in the
manifest are authoritative. The neutral Natural Vibrato row reuses the
all-zero Flex timeline bounce, as it is the same plug-in configuration.

The available target set is complete for the measured positions: Flex at
Retune 0 and 10 ms, Humanize 0/25/50/75/100 ms, and Natural Vibrato
-12/-9/-6/0/+3/+6/+9/+12 ms. The original planned 25-ms Flex sweep is not
present, but it is not a missing value from the recorded 0-ms/10-ms test set.

## Planned bounce matrix

Put the rendered Artist WAVs in `artist-renders/` using these names:

| Control | Input files | Values | Output name suffix |
| --- | --- | --- | --- |
| Flex-Tune | every `flex_*.wav` | 0, 25, 50, 75, 100 at Retune 0 and 10 ms | `_artist_flexNN_retuneNN.wav` |
| Humanize | every `humanize_*.wav` | 0, 25, 50, 75, 100 | `_artist_humanizeNN.wav` |
| Natural Vibrato | every `natural_vibrato_*.wav` | -12, -9, -6, 0, +3, +6, +9, +12 ms | `_artist_naturalvibratoNN.wav` |

Example: `flex_fs4_plus30ct_3s_artist_flex50_retune25.wav`.

## Faster Logic route

Use the files in `logic-timelines/` rather than bouncing every short input
separately. Each timeline has the relevant stimuli separated by 350 ms of
silence, so Artist state is released between them. This reduces the first
calibration pass to 15 bounces:

- `flex_timeline.wav`: bounce five times at Flex 0/25/50/75/100 with Retune
  0 ms, then repeat five times at Retune 25 ms.
- `humanize_timeline.wav`: bounce five times at Humanize 0/25/50/75/100.
- `natural_vibrato_timeline.wav`: bounce five times at Natural Vibrato
  0/25/50/75/100.

Name a timeline bounce, for example, `flex_timeline_artist_flex50_retune25.wav`.
The segment order is exactly the order documented by its matching `.ffconcat`
file. Start with values 0/50/100 if a fast coarse curve is enough; add 25/75
only where the measured curve is nonlinear.

## What is measured

- **Flex-Tune:** steady residual cents as a function of input error. The
  `plus` and `minus` files reveal whether the curve is symmetric.
- **Humanize:** time before relaxation, retained 5 Hz modulation depth, and
  whether slow non-periodic drift continues to be corrected. The 200 ms
  note is a negative control: Humanize should not change an attack/short
  note.
- **Natural Vibrato:** output modulation depth versus input depth and rate.
  It is measured only after the note has settled; attack samples are ignored.

Run `VibeAutotuneArtistControlStimuli` to regenerate `input/` if needed.

## Re-measure VibeAutotune against every target

After building `VibeAutotuneSignalChainDiagnostic` and
`VibeAutotuneTargetReferenceMatch`, run:

```zsh
scripts/measure_artist_control_manifest.zsh renders/control-manifest-baseline
```

It reads only `verified` rows from `TARGET_MANIFEST.csv`, renders the exact
Vibe parameter configuration, preserves per-case WAV/telemetry/metric files,
and writes one comparable `summary.csv`. Alongside waveform/spectral metrics,
it exports target/candidate F0 contours and reports latency-compensated F0
RMS cents. It is the required baseline before changing a control curve.

`BENCHMARK_INPUT_PROVENANCE.csv` is the companion raw-input gate. The script
SHA-256-checks both the Artist target and the raw input before rendering. By
default it renders only rows with a `verified` raw source and writes excluded
rows to `skipped.csv`. The original bundled `raw_logic_matched.wav` remains an
inferred extract and is retained only for historical comparison. On
2026-09-30 the user supplied the original 182.101187-second `hook 2.wav` and
identified the first voice target region as the source. Target-envelope audit
places its selection boundary at sample 1308000 (27.250000 seconds); the next
524576 samples are now the verified input for the 200/300/400-ms rows. The
27.000-second slice belongs to the longer 0-ms benchmark and fails the
high-Retune envelope comparison. Never substitute the later look-alike regions
in that track. `VIBE_ALLOW_PROVISIONAL_INPUTS=1` is permitted only for
explicitly labelled exploration.

## Local alignment audit

The whole-file `2048`-sample alignment is useful for stable waveform checks,
but it is not a valid phase objective for every part of the concatenated
Natural-Vibrato timeline.  The recorded Artist bounces exhibit the same
time-varying local offset at Natural `-12`, `-9`, and `-6`; it is therefore a
timeline/reference characteristic, not three separate knob laws. Before
changing a renderer or control law, inspect local F0 alignment:

```zsh
python3 scripts/measure_local_pitch_alignment.py \
  renders/control-manifest-phasefit-final-20260929/vibrato_vminus12/target_contour.csv \
  renders/natural-negative-windowed-quadrature-20260929/natural_-12/contour.csv \
  renders/local-alignment-natural-20260929/natural_minus12.csv
```

The report uses 0.5-s windows and scans offsets from 1024 to 4096 samples.
Do not attempt to compensate those local offsets by changing Retune, target
selection, or a global renderer latency. Fit a control law only inside a
region whose local offset is stable; otherwise the metric is conflating the
Artist/reference timing with pitch contour error.

## Four-control transition audit

Retune, Flex-Tune, Humanize and Natural Vibrato are one control vector, not
four independent post-processes. A transition result is identified by the
ordered tuple `retune/flex/humanize/natural`; never pool rows across different
tuples when computing timing percentiles. In particular, Humanize and Natural
Vibrato depend on the same sustained residual that Retune and Flex change.

`VibeAutotuneSignalChainDiagnostic` now writes the four requested control
values plus portamento, target-trajectory and renderer state into
`full_default_intervals.csv`. Attach that state to the local response report
with:

```zsh
python3 scripts/measure_retune_transition_timing.py \
  RAW_CONTOUR.csv ARTIST_CONTOUR.csv VIBE_CONTOUR.csv OUT.csv 1280 \
  full_default_intervals.csv
```

The transition window ends at the next stable raw transition (or 800 ms,
whichever comes first). This prevents a later note from being misreported as
the response to the current note. Unmatched rows are retained with an explicit
`match_status`; they must not be silently dropped from an interaction report.

The verified Artist manifest is the coordinated calibration suite. Every row
specifies the full Retune/Flex/Humanize/Natural tuple, including neutral
members, and must be rendered through the same signal path. A family label
describes the sweep that produced the target; it does not authorize fitting
that control as an isolated post-process. Add future combined-control Artist
bounces with all four values populated before using them as calibration
targets.

## Coverage boundary: do not invent combined-control targets

`VibeAutotune-ArtistControlTargets-verified-20260929.zip` contains the
verified **single-control sweeps** and their neutral baselines. It does not
contain an official Artist reference for arbitrary four-control combinations.
For example, neither `Retune 300 + Humanize 50` nor `Retune 400 + Natural
Vibrato -6` is currently a verified target in `TARGET_MANIFEST.csv`.

Do not approximate a missing combination by combining measurements from
separate WAVs, and do not mark such an output as matched to Artist. To
calibrate one, bounce that exact tuple through Artist with the fixed settings
above, add its WAV and SHA-256 to the manifest, then re-run the complete
measurement script.
