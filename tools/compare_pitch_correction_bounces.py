#!/usr/bin/env python3
"""Offline black-box comparison for two latency-aligned pitch-correction bounces."""

import argparse
import csv
import json
from pathlib import Path

import numpy as np
from scipy.io import wavfile
from scipy.signal import butter, correlate, find_peaks, sosfiltfilt, stft


def read_mono(path: Path):
    sample_rate, samples = wavfile.read(path)
    if np.issubdtype(samples.dtype, np.integer):
        samples = samples.astype(np.float64) / np.iinfo(samples.dtype).max
    else:
        samples = samples.astype(np.float64)
    if samples.ndim == 2:
        samples = samples.mean(axis=1)
    return sample_rate, samples


def normalized_lag(reference, candidate, max_lag):
    correlation = correlate(candidate, reference, mode="full", method="fft")
    lags = np.arange(-len(reference) + 1, len(candidate))
    allowed = np.abs(lags) <= max_lag
    lag = int(lags[allowed][np.argmax(correlation[allowed])])
    return lag


def align(reference, candidate, lag):
    # Positive lag means candidate is later than reference.
    if lag > 0:
        candidate = candidate[lag:]
    elif lag < 0:
        reference = reference[-lag:]
    length = min(len(reference), len(candidate))
    return reference[:length], candidate[:length]


def local_lags(reference, candidate, sample_rate):
    window = int(0.250 * sample_rate)
    hop = int(0.125 * sample_rate)
    maximum = int(0.020 * sample_rate)
    rows = []
    for begin in range(0, min(len(reference), len(candidate)) - window + 1, hop):
        r = reference[begin:begin + window]
        c = candidate[begin:begin + window]
        lag = normalized_lag(r, c, maximum)
        rows.append((begin / sample_rate, lag))
    return rows


def pitch_track(signal, sample_rate):
    frame = 1024
    hop = 64
    low_period = int(sample_rate / 500.0)
    high_period = int(sample_rate / 80.0)
    window = np.hanning(frame)
    result = []
    for start in range(0, len(signal) - frame + 1, hop):
        x = signal[start:start + frame]
        x = (x - np.mean(x)) * window
        energy = np.dot(x, x)
        if energy < 1e-8:
            result.append((start + frame // 2, np.nan, 0.0))
            continue
        autocorrelation = correlate(x, x, mode="full", method="fft")[frame - 1:]
        autocorrelation /= autocorrelation[0] + 1e-18
        upper = min(high_period, len(autocorrelation) - 2)
        peak_range = autocorrelation[low_period:upper]
        peaks, _ = find_peaks(peak_range)
        if len(peaks) == 0:
            result.append((start + frame // 2, np.nan, 0.0))
            continue
        peak = int(peaks[np.argmax(peak_range[peaks])]) + low_period
        confidence = float(autocorrelation[peak])
        if peak <= 0 or confidence < 0.35:
            result.append((start + frame // 2, np.nan, confidence))
            continue
        left, centre, right = autocorrelation[peak - 1:peak + 2]
        denominator = left - 2.0 * centre + right
        offset = 0.5 * (left - right) / denominator if abs(denominator) > 1e-12 else 0.0
        frequency = sample_rate / (peak + offset)
        result.append((start + frame // 2, frequency, confidence))
    return np.asarray(result, dtype=np.float64)


def cents(a, b):
    return 1200.0 * np.log2(a / b)


def spectral_features(signal, sample_rate):
    _, _, spectrum = stft(signal, fs=sample_rate, nperseg=1024, noverlap=768,
                          boundary=None, padded=False)
    power = np.abs(spectrum) ** 2 + 1e-18
    frequencies = np.linspace(0.0, sample_rate * 0.5, power.shape[0])[:, None]
    centroid = (frequencies * power).sum(axis=0) / power.sum(axis=0)
    flatness = np.exp(np.mean(np.log(power), axis=0)) / np.mean(power, axis=0)
    high = power[frequencies[:, 0] >= 3000.0].sum(axis=0) / power.sum(axis=0)
    return centroid, flatness, high


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("artist", type=Path)
    parser.add_argument("candidate", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)

    sample_rate, artist = read_mono(args.artist)
    candidate_rate, candidate = read_mono(args.candidate)
    if sample_rate != candidate_rate:
        raise RuntimeError("sample-rate mismatch")

    lag = normalized_lag(artist, candidate, int(0.100 * sample_rate))
    artist, candidate = align(artist, candidate, lag)
    residual = candidate - artist
    rms = lambda x: float(np.sqrt(np.mean(x * x)))
    aligned_nrmse = rms(residual) / max(rms(artist), 1e-12)
    envelope_window = int(0.020 * sample_rate)
    envelope = lambda x: np.sqrt(np.convolve(x * x, np.ones(envelope_window) / envelope_window, mode="same"))
    artist_envelope, candidate_envelope = envelope(artist), envelope(candidate)
    envelope_correlation = float(np.corrcoef(artist_envelope, candidate_envelope)[0, 1])

    artist_pitch = pitch_track(artist, sample_rate)
    candidate_pitch = pitch_track(candidate, sample_rate)
    usable = np.isfinite(artist_pitch[:, 1]) & np.isfinite(candidate_pitch[:, 1]) \
             & (artist_pitch[:, 2] >= 0.55) & (candidate_pitch[:, 2] >= 0.55)
    pitch_error = cents(candidate_pitch[usable, 1], artist_pitch[usable, 1])
    pitch_times = artist_pitch[usable, 0] / sample_rate
    order = np.argsort(np.abs(pitch_error))[::-1]

    a_centroid, a_flatness, a_high = spectral_features(artist, sample_rate)
    c_centroid, c_flatness, c_high = spectral_features(candidate, sample_rate)
    count = min(len(a_centroid), len(c_centroid))

    highpass = butter(4, 3000.0, fs=sample_rate, output="sos")
    a_hp, c_hp = sosfiltfilt(highpass, artist), sosfiltfilt(highpass, candidate)
    diff_a, diff_c = np.abs(np.diff(artist)), np.abs(np.diff(candidate))
    metrics = {
        "sample_rate": sample_rate,
        "global_alignment_candidate_minus_artist_samples": lag,
        "aligned_samples": len(artist),
        "aligned_duration_seconds": len(artist) / sample_rate,
        "waveform_nrmse": aligned_nrmse,
        "envelope_correlation": envelope_correlation,
        "artist_rms": rms(artist),
        "candidate_rms": rms(candidate),
        "artist_peak": float(np.max(np.abs(artist))),
        "candidate_peak": float(np.max(np.abs(candidate))),
        "artist_max_sample_step": float(np.max(diff_a)),
        "candidate_max_sample_step": float(np.max(diff_c)),
        "artist_highpass_rms_3khz": rms(a_hp),
        "candidate_highpass_rms_3khz": rms(c_hp),
        "pitch_valid_frames": int(np.sum(usable)),
        "pitch_error_median_cents": float(np.median(pitch_error)),
        "pitch_error_median_absolute_cents": float(np.median(np.abs(pitch_error))),
        "pitch_error_p90_absolute_cents": float(np.quantile(np.abs(pitch_error), 0.90)),
        "pitch_error_p99_absolute_cents": float(np.quantile(np.abs(pitch_error), 0.99)),
        "spectral_centroid_delta_hz": float(np.mean(c_centroid[:count] - a_centroid[:count])),
        "spectral_flatness_delta": float(np.mean(c_flatness[:count] - a_flatness[:count])),
        "high_band_energy_fraction_delta": float(np.mean(c_high[:count] - a_high[:count])),
    }
    with (args.output / "metrics.json").open("w") as handle:
        json.dump(metrics, handle, indent=2)
    with (args.output / "pitch_outliers.csv").open("w", newline="") as handle:
        writer = csv.writer(handle)
        writer.writerow(["time_seconds", "artist_hz", "candidate_hz", "candidate_minus_artist_cents"])
        for index in order[:200]:
            writer.writerow([pitch_times[index], artist_pitch[usable, 1][index],
                             candidate_pitch[usable, 1][index], pitch_error[index]])
    with (args.output / "local_lags.csv").open("w", newline="") as handle:
        writer = csv.writer(handle)
        writer.writerow(["time_seconds", "candidate_minus_artist_samples"])
        writer.writerows(local_lags(artist, candidate, sample_rate))
    print(json.dumps(metrics, indent=2))


if __name__ == "__main__":
    main()
