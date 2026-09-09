#!/bin/zsh
set -euo pipefail

if (( $# != 2 )); then
    print -u2 "usage: score_target_pitch.zsh <target-wav> <candidate-wav>"
    exit 2
fi

script_dir=${0:A:h}
repo_dir=${script_dir:h}
exporter=${repo_dir}/build/VibeAutotunePitchContourExport
if [[ ! -x ${exporter} ]]; then
    print -u2 "missing pitch exporter; build VibeAutotunePitchContourExport first"
    exit 3
fi

score_dir=$(mktemp -d /private/tmp/vibeautotune-target-score.XXXXXX)
trap 'rm -rf -- "$score_dir"' EXIT
"${exporter}" "$1" > "${score_dir}/target.csv"
"${exporter}" "$2" > "${score_dir}/candidate.csv"

# 250 ms buckets expose the exact transitions that a global average hides.
# The first line is a total; subsequent lines are time-local objectives for
# an optimizer to improve without regressing the sustained vowels.
awk -F, '
NR == FNR {
    if (FNR > 1 && $2 != "nan") target[$1] = $2
    next
}
FNR > 1 && $2 != "nan" && ($1 in target) {
    difference = 1200 * log (target[$1] / $2) / log (2)
    if (difference < 0) difference = -difference
    bucket = int ($1 / 12000)
    sum[bucket] += difference
    count[bucket]++
    if (difference > maximum[bucket]) maximum[bucket] = difference
    total += difference
    frames++
    if (difference > totalMaximum) totalMaximum = difference
}
END {
    printf "total,mean_cents=%.3f,max_cents=%.3f,frames=%d\n", total / frames, totalMaximum, frames
    for (bucket in count)
        printf "bucket,%.2f-%.2fs,mean_cents=%.3f,max_cents=%.3f,frames=%d\n", bucket * 0.25, (bucket + 1) * 0.25, sum[bucket] / count[bucket], maximum[bucket], count[bucket]
}' "${score_dir}/target.csv" "${score_dir}/candidate.csv" | sort -t, -k1,1 -k2,2n
