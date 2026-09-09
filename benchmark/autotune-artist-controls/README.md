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

## Bounce matrix

Put the rendered Artist WAVs in `artist-renders/` using these names:

| Control | Input files | Values | Output name suffix |
| --- | --- | --- | --- |
| Flex-Tune | every `flex_*.wav` | 0, 25, 50, 75, 100 at Retune 0 and 25 ms | `_artist_flexNN_retuneNN.wav` |
| Humanize | every `humanize_*.wav` | 0, 25, 50, 75, 100 | `_artist_humanizeNN.wav` |
| Natural Vibrato | every `natural_vibrato_*.wav` | 0, 25, 50, 75, 100 | `_artist_naturalvibratoNN.wav` |

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
