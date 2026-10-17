#!/usr/bin/env python3
"""Measure per-transition pitch response against an Artist bounce.

Usage:
  measure_retune_transition_timing.py RAW.csv ARTIST.csv CANDIDATE.csv OUT.csv \
      [candidate-latency-samples] [VIBE_INTERVALS.csv]

The input, Artist and candidate contours use the two-column output of
VibeAutotuneIndependentPitchContour.  A transition is a stable change of
nearest legal F# Natural Minor target in the raw contour.  The report records
when Artist and the candidate first move 15 cents from their pre-transition
pitch in the raw transition direction.  It deliberately reports observations,
not a claim about either product's internal target state.

When a Vibe signal-chain intervals CSV is supplied, the report also attaches
the complete four-control setting and selected trajectory/renderer state at
the raw transition and candidate response. This is diagnostic context only;
it is never used to decide where either response occurred.
"""

import bisect
import csv
import math
import sys

SAMPLE_RATE = 48000
LEGAL_PITCH_CLASSES = {1, 2, 4, 6, 8, 9, 11}  # F# Natural Minor
MIN_STABLE_FRAMES = 2
MIN_RESPONSE_CENTS = 15.0
RAW_DIRECTIONAL_MOTION_CENTS = 15.0
RAW_DIRECTIONAL_MOTION_SAMPLES = 3 * 256
REFRACTORY_SAMPLES = int(0.080 * SAMPLE_RATE)

CONTROL_FIELDS = (
    "retuneSpeedMs",
    "flexTunePercent",
    "humanizePercent",
    "naturalVibratoPercent",
)

STATE_FIELDS = (
    "committedTargetMidi",
    "targetChanged",
    "trajectoryTransitionActive",
    "trajectoryTransitionProgress",
    "trajectoryTransitionDurationMs",
    "trajectoryPortamentoActive",
    "trajectoryUsesPortamentoTarget",
    "desiredCorrectionCents",
    "retuneCorrectionCents",
    "humanizeWeight",
    "vibratoContributionCents",
    "sourceControlTimestamp",
    "sourceControlTargetMidi",
    "sourceControlPitchRatio",
    "olaDominantGrainTargetMidi",
    "olaDominantGrainRatio",
    "rendererStepCents",
)


def read_contour(path):
    result = {}
    with open(path, newline="") as source:
        for row in csv.DictReader(source):
            hz = float(row["independent_f0_hz"])
            if math.isfinite(hz) and hz > 0.0:
                result[int(row["sample"])] = 1200.0 * math.log2(hz / 440.0)
    return result


def read_telemetry(path):
    rows = []
    with open(path, newline="") as source:
        reader = csv.DictReader(source)
        required = {"sampleIndex", *CONTROL_FIELDS, *STATE_FIELDS}
        missing = sorted(required.difference(reader.fieldnames or ()))
        if missing:
            raise ValueError(f"{path}: missing telemetry columns: {', '.join(missing)}")
        for row in reader:
            row["sampleIndex"] = int(row["sampleIndex"])
            rows.append(row)
    rows.sort(key=lambda row: row["sampleIndex"])
    return [row["sampleIndex"] for row in rows], rows


def telemetry_at_or_before(telemetry, sample):
    if telemetry is None:
        return None
    samples, rows = telemetry
    index = bisect.bisect_right(samples, sample) - 1
    return rows[index] if index >= 0 else None


def telemetry_values(row, fields):
    if row is None:
        return [""] * len(fields)
    return [row[field] for field in fields]


def canonical_control(value):
    number = float(value)
    if abs(number) < 0.0005:
        number = 0.0
    return f"{number:.3f}".rstrip("0").rstrip(".")


def nearest_legal_midi(cents):
    midi = 69.0 + cents / 100.0
    choices = [note for note in range(0, 128) if note % 12 in LEGAL_PITCH_CLASSES]
    return min(choices, key=lambda note: abs(note - midi))


def raw_transitions(raw):
    samples = sorted(raw)
    result = []
    current = None
    pending = None
    pending_frames = 0
    last_transition = -REFRACTORY_SAMPLES
    for sample in samples:
        note = nearest_legal_midi(raw[sample])
        if current is None:
            current = note
            continue
        if note == current:
            pending = None
            pending_frames = 0
            continue
        if pending == note:
            pending_frames += 1
        else:
            pending = note
            pending_frames = 1
        if pending_frames >= MIN_STABLE_FRAMES and sample - last_transition >= REFRACTORY_SAMPLES:
            result.append((sample, current, note))
            last_transition = sample
            current = note
            pending = None
            pending_frames = 0
    return result


def contour_at_or_before(contour, sample):
    available = [key for key in contour if key <= sample]
    return contour[max(available)] if available else None


def first_response(contour, observation_start, observation_end, direction, baseline, offset=0):
    for sample in sorted(contour):
        timeline_sample = sample - offset
        if timeline_sample < observation_start:
            continue
        if timeline_sample >= observation_end:
            break
        if (contour[sample] - baseline) * direction >= MIN_RESPONSE_CENTS:
            return timeline_sample
    return None


def raw_directional_motion(raw, transition_sample, direction):
    """Return the short-horizon raw motion that supports a legal-note event.

    Independent F0 can emit a plausible but unrelated scale note during a
    consonant or an unvoiced gap.  Quantising that single estimate makes a
    false "transition" which cannot be used to tune Retune.  This read-only
    quality measure requires the raw contour itself to travel at least 15
    cents toward the newly selected note over the next three 256-sample hops.
    It deliberately does not alter transition detection or either audio file.
    """
    before = contour_at_or_before(raw, transition_sample - 256)
    after = contour_at_or_before(raw, transition_sample + RAW_DIRECTIONAL_MOTION_SAMPLES)
    if before is None or after is None:
        return None
    return (after - before) * direction


def main():
    if len(sys.argv) not in (5, 6, 7):
        raise SystemExit(__doc__)
    raw = read_contour(sys.argv[1])
    artist = read_contour(sys.argv[2])
    candidate = read_contour(sys.argv[3])
    output_path = sys.argv[4]
    candidate_latency = int(sys.argv[5]) if len(sys.argv) == 6 else 1280
    if len(sys.argv) == 7:
        candidate_latency = int(sys.argv[5])
    telemetry = read_telemetry(sys.argv[6]) if len(sys.argv) == 7 else None

    transition_state_fields = tuple(f"transition_{field}" for field in STATE_FIELDS)
    response_state_fields = tuple(f"response_{field}" for field in STATE_FIELDS)

    with open(output_path, "w", newline="") as output:
        writer = csv.writer(output)
        writer.writerow(("raw_transition_sample", "raw_transition_seconds", "from_midi", "to_midi",
                         "raw_directional_motion_16ms_cents", "raw_transition_quality",
                         "observation_end_sample", "observation_window_ms",
                         "artist_response_sample", "artist_response_delay_ms",
                         "candidate_response_sample", "candidate_response_delay_ms",
                         "candidate_minus_artist_ms", "match_status", "interaction_id", *CONTROL_FIELDS,
                         *transition_state_fields, *response_state_fields))
        transitions = raw_transitions(raw)
        for index, (sample, before, after) in enumerate(transitions):
            direction = 1.0 if after > before else -1.0
            directional_motion = raw_directional_motion(raw, sample, direction)
            transition_quality = ("coherent_raw_motion"
                                  if directional_motion is not None
                                  and directional_motion >= RAW_DIRECTIONAL_MOTION_CENTS
                                  else "weak_or_opposed_raw_motion")
            maximum_end = sample + int(0.800 * SAMPLE_RATE)
            observation_end = min(maximum_end, transitions[index + 1][0]) \
                if index + 1 < len(transitions) else maximum_end
            artist_baseline = contour_at_or_before(artist, sample - 256)
            candidate_baseline = contour_at_or_before(candidate, sample + candidate_latency - 256)
            artist_response = None if artist_baseline is None else first_response(
                artist, sample, observation_end, direction, artist_baseline)
            candidate_response = None if candidate_baseline is None else first_response(
                candidate, sample, observation_end, direction, candidate_baseline, candidate_latency)
            artist_delay = None if artist_response is None else 1000.0 * (artist_response - sample) / SAMPLE_RATE
            candidate_delay = None if candidate_response is None else 1000.0 * (candidate_response - sample) / SAMPLE_RATE
            difference = None if artist_delay is None or candidate_delay is None else candidate_delay - artist_delay
            transition_telemetry = telemetry_at_or_before(telemetry, sample + candidate_latency)
            response_telemetry = telemetry_at_or_before(
                telemetry,
                sample + candidate_latency if candidate_response is None
                else candidate_response + candidate_latency)
            controls = telemetry_values(transition_telemetry, CONTROL_FIELDS)
            controls = [canonical_control(value) for value in controls] if transition_telemetry else controls
            interaction_id = "" if transition_telemetry is None else "/".join(controls)
            if artist_response is not None and candidate_response is not None:
                match_status = "matched"
            elif artist_response is None and candidate_response is None:
                match_status = "neither_responded_before_next_transition"
            elif artist_response is None:
                match_status = "artist_no_response_before_next_transition"
            else:
                match_status = "candidate_no_response_before_next_transition"
            writer.writerow((sample, f"{sample / SAMPLE_RATE:.6f}", before, after,
                             f"{directional_motion:.3f}" if directional_motion is not None else "",
                             transition_quality,
                             observation_end,
                             f"{1000.0 * (observation_end - sample) / SAMPLE_RATE:.3f}",
                             artist_response if artist_response is not None else "",
                             f"{artist_delay:.3f}" if artist_delay is not None else "",
                             candidate_response if candidate_response is not None else "",
                             f"{candidate_delay:.3f}" if candidate_delay is not None else "",
                             f"{difference:.3f}" if difference is not None else "",
                             match_status, interaction_id, *controls,
                             *telemetry_values(transition_telemetry, STATE_FIELDS),
                             *telemetry_values(response_telemetry, STATE_FIELDS)))


if __name__ == "__main__":
    main()
