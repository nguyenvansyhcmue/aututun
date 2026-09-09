#!/bin/zsh
# Measure an offline Auto-Tune render of the C5 -> B4 stimulus. The input
# changes at 1.000 s; output threshold times include plug-in/host latency.
set -euo pipefail

if (( $# != 1 )); then
  print -u2 "usage: $0 rendered.wav"
  exit 64
fi

root=${0:A:h:h}
exporter="$root/build/VibeAutotunePitchContourExport"
if [[ ! -x "$exporter" ]]; then
  print -u2 "missing $exporter; build target VibeAutotunePitchContourExport first"
  exit 69
fi

"$exporter" "$1" | awk -F, '
NR > 1 && $3 == 1 {
    sample[++count] = $1; f0[count] = $2;
    # Artist has a small static calibration offset. Use the rendered C5,
    # rather than ideal 523.251 Hz, as the endpoint for its dynamic curve.
    if ($1 >= 24000 && $1 < 43000) { baseline += $2; baselineCount++; }
}
END {
    if (! baselineCount) exit 1;
    target = baseline / baselineCount; onset = 48000;
    minIndex = 0;
    for (i = 1; i <= count; i++)
        if ((sample[i] >= onset && sample[i] <= onset + 48000) && (! minIndex || f0[i] < f0[minIndex])) minIndex = i;
    if (! minIndex || target - f0[minIndex] < 1.0) {
        print "no measurable departure from rendered target";
        exit 0;
    }
    print "event,output_sample,output_ms,ms_after_input_step,ms_after_trough,f0_hz";
    printf "rendered_target,,,,,%.6f\n", target;
    printf "trough,%d,%.3f,%.3f,0.000,%.6f\n", sample[minIndex], sample[minIndex] / 48.0,
           (sample[minIndex] - onset) / 48.0, f0[minIndex];
    for (j = 1; j <= 4; j++) {
        threshold = (j == 1 ? 0.10 : j == 2 ? 0.50 : j == 3 ? 0.90 : 0.99);
        level = f0[minIndex] + (target - f0[minIndex]) * threshold;
        for (i = minIndex; i <= count; i++)
            if (f0[i] >= level) {
                printf "recovery_%.2f,%d,%.3f,%.3f,%.3f,%.6f\n", threshold, sample[i], sample[i] / 48.0,
                       (sample[i] - onset) / 48.0, (sample[i] - sample[minIndex]) / 48.0, f0[i];
                break;
            }
    }
}'
