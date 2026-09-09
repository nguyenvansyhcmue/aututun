# F# Minor Alto Reference

This benchmark contains the voiced opening segment shared by the supplied
reference render and its raw source. Both files begin on the same timeline
and are exactly 11.034500 seconds at 48 kHz.

| File | Role | Format |
| --- | --- | --- |
| `raw-input.wav` | Source vocal for the plugin | mono, 24-bit PCM |
| `target-output.wav` | Reference result | dual-mono stereo, 24-bit PCM |

The first two confirmed silent gaps align closely:

| Gap | Raw input | Target output |
| --- | ---: | ---: |
| 1 | 0.300–0.405 s | 0.300–0.406 s |
| 2 | 6.275–6.604 s | 6.278–6.608 s |

Use this fixed preset for every comparison: Alto, F# Natural Minor, Tracking
50, Retune Speed 0 ms, Flex-Tune 0%, Humanize 0%, and Natural Vibrato 0%.

The production renderer reports a fixed 2048-sample (42.67 ms) latency to
the host. Compare a direct offline render with `candidate-alignment-samples`
set to `2048`, or compensate that delay before comparing WAV files.
