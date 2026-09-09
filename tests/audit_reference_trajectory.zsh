#!/bin/zsh
set -euo pipefail

if (( $# != 4 )); then
    print -u2 "usage: audit_reference_trajectory.zsh <raw-wav> <reference-wav> <project-wav> <signal-chain-csv>"
    exit 2
fi

script_dir=${0:A:h}
repo_dir=${script_dir:h}
exporter=${repo_dir}/build/VibeAutotunePitchContourExport
out_dir=${repo_dir}/build/vocal-preview-hook2212/forensics
mkdir -p "${out_dir}"

# Measured from the full vocal energy envelope: reference leads the raw
# capture by 80 ms (3,840 samples at 48 kHz).  The former +61 sample value
# compared different phonemes at every luyến and made pitch scores invalid.
# These are analysis-only offsets; no WAV is rewritten.
reference_offset=-3840
project_offset=256
print "stream,offset_samples,offset_ms" > "${out_dir}/alignment.csv"
print "reference,${reference_offset},$(awk 'BEGIN { printf "%.6f", 1000 * -3840 / 48000 }')" >> "${out_dir}/alignment.csv"
print "project,${project_offset},$(awk 'BEGIN { printf "%.6f", 1000 * 256 / 48000 }')" >> "${out_dir}/alignment.csv"

"${exporter}" "$1" > "${out_dir}/raw_pitch.csv"
"${exporter}" "$2" > "${out_dir}/reference_pitch.csv"
"${exporter}" "$3" > "${out_dir}/project_pitch.csv"

awk -F, -v refOffset="${reference_offset}" -v projectOffset="${project_offset}" '
function cents (hz) { return 1200 * log(hz / 440) / log(2) }
function interpolation (values, position, lower, upper, fraction) {
    lower = int(position / 512) * 512
    upper = lower + 512
    fraction = (position - lower) / 512
    if (lower in values && upper in values)
        return exp((1 - fraction) * log(values[lower]) + fraction * log(values[upper]))
    if (lower in values) return values[lower]
    if (upper in values) return values[upper]
    return 0
}
FILENAME == ARGV[1] { if (FNR > 1 && $2 != "nan") ref[$1] = $2; next }
FILENAME == ARGV[2] { if (FNR > 1 && $2 != "nan") project[$1] = $2; next }
FILENAME == ARGV[3] { if (FNR > 1 && $2 != "nan") raw[$1] = $2; next }
FILENAME == ARGV[4] {
    if (FNR > 1) {
        currentTarget[$1] = $33
        candidateTarget[$1] = $34
        desiredCorrection[$1] = $16
        rendererPreGuard[$1] = $21
        rendererPostGuard[$1] = $22
        rendererSource[$1] = $23
        deliveredCorrection[$1] = $24
        rendererInput[$1] = $25
        transitionActive[$1] = $35
        transitionProgress[$1] = $36
        latticeReset[$1] = $37
        dryGuard[$1] = $38
        reentryHold[$1] = $39
    }
    next
}
END {
    print "time_ms,raw_f0_hz,reference_f0_hz,project_f0_hz,raw_cents,reference_cents,project_cents,reference_correction_cents,project_correction_cents,current_target_cents,candidate_target_cents,desired_correction_cents,renderer_pre_guard_correction_cents,renderer_post_guard_correction_cents,renderer_correction_source,renderer_input_correction_cents,delivered_correction_cents,transition_active,transition_progress,lattice_reset,dry_guard_active,reentry_hold_active"
    # The diagnostic stream runs every 128 samples. It is the canonical
    # timeline for the control/renderer hand-off; F0 streams are interpolated
    # onto it. A diagnostic at S produces audible project audio at S + 256.
    for (sample in desiredCorrection) {
        # Each output stream is measured at its physical WAV timestamp. Move
        # the reference/project F0 back to the raw timeline using the measured
        # onset offsets. F0 is log-interpolated between its 512-sample frames.
        rawHz = interpolation(raw, sample)
        referenceHz = interpolation(ref, sample + refOffset)
        projectHz = interpolation(project, sample + projectOffset)
        if (rawHz == 0 || referenceHz == 0 || projectHz == 0) continue
        rawCents = cents(rawHz)
        referenceCents = cents(referenceHz)
        projectCents = cents(projectHz)
        printf "%.6f,%.9f,%.9f,%.9f,%.6f,%.6f,%.6f,%.6f,%.6f,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n", sample * 1000 / 48000, rawHz, referenceHz, projectHz, rawCents, referenceCents, projectCents, referenceCents - rawCents, projectCents - rawCents, currentTarget[sample], candidateTarget[sample], desiredCorrection[sample], rendererPreGuard[sample], rendererPostGuard[sample], rendererSource[sample], rendererInput[sample], deliveredCorrection[sample], transitionActive[sample], transitionProgress[sample], latticeReset[sample], dryGuard[sample], reentryHold[sample]
    }
}' "${out_dir}/reference_pitch.csv" "${out_dir}/project_pitch.csv" "${out_dir}/raw_pitch.csv" "$4" | sort -t, -k1,1n > "${out_dir}/aligned_trajectory.csv"

print "alignment=${out_dir}/alignment.csv"
print "trajectory=${out_dir}/aligned_trajectory.csv"
