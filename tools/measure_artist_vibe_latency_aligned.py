#!/usr/bin/env python3
"""Compare Artist and Vibe pitch after independently aligning both to raw.

Waveform cross-correlation is deliberately avoided: different PSOLA phase and
grain choices can make sample-level correlation poor even when pitch is right.
"""
import argparse
import json
from pathlib import Path

import numpy as np
from scipy.io import wavfile
from scipy.signal import correlate

from compare_pitch_correction_bounces import pitch_track, cents


def read_mono(path):
    rate, x = wavfile.read(path)
    if np.issubdtype(x.dtype, np.integer):
        x = x.astype(np.float64) / np.iinfo(x.dtype).max
    else:
        x = x.astype(np.float64)
    return rate, x.mean(axis=1) if x.ndim == 2 else x


def rms_envelope(x, window):
    return np.sqrt(np.convolve(x * x, np.full(window, 1.0 / window), mode="same"))


def envelope_delay(raw, rendered, sample_rate):
    # Positive return means rendered is later than raw. Limit to 100 ms.
    a, b = rms_envelope(raw, int(.020 * sample_rate)), rms_envelope(rendered, int(.020 * sample_rate))
    values = correlate(b, a, mode="full", method="fft")
    lags = np.arange(-len(a) + 1, len(b))
    keep = np.abs(lags) <= int(.100 * sample_rate)
    return int(lags[keep][np.argmax(values[keep])])


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("raw", type=Path)
    parser.add_argument("artist", type=Path)
    parser.add_argument("vibe", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)

    rate, raw = read_mono(args.raw)
    artist_rate, artist = read_mono(args.artist)
    vibe_rate, vibe = read_mono(args.vibe)
    if rate != artist_rate or rate != vibe_rate:
        raise RuntimeError("sample-rate mismatch")

    artist_delay = envelope_delay(raw, artist, rate)
    vibe_delay = envelope_delay(raw, vibe, rate)
    at = pitch_track(artist, rate)
    vt = pitch_track(vibe, rate)
    artist_time = at[:, 0] / rate - artist_delay / rate
    vibe_time = vt[:, 0] / rate - vibe_delay / rate
    valid_a = np.isfinite(at[:, 1]) & (at[:, 2] >= .55)
    valid_v = np.isfinite(vt[:, 1]) & (vt[:, 2] >= .55)
    # Interpolate only across Vibe frames deemed reliable.
    queried_vibe = np.interp(artist_time[valid_a], vibe_time[valid_v], vt[valid_v, 1], left=np.nan, right=np.nan)
    a = at[valid_a, 1]
    usable = np.isfinite(queried_vibe) & (a > 0) & (queried_vibe > 0)
    error = cents(queried_vibe[usable], a[usable])
    notes_match = np.rint(69 + 12 * np.log2(queried_vibe[usable] / 440.0)) == np.rint(69 + 12 * np.log2(a[usable] / 440.0))
    result = {
        "sample_rate": rate,
        "artist_delay_vs_raw_samples": artist_delay,
        "vibe_delay_vs_raw_samples": vibe_delay,
        "artist_vibe_relative_delay_samples": vibe_delay - artist_delay,
        "valid_pitch_frames": int(error.size),
        "same_quantized_note_fraction": float(np.mean(notes_match)),
        "median_signed_cents": float(np.median(error)),
        "median_absolute_cents": float(np.median(np.abs(error))),
        "p90_absolute_cents": float(np.quantile(np.abs(error), .90)),
        "p99_absolute_cents": float(np.quantile(np.abs(error), .99)),
    }
    (args.output / "latency_aligned_metrics.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
