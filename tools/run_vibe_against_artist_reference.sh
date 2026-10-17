#!/bin/zsh
# Reproducible comparison when Artist renders were bounced in Logic.  This
# avoids requiring the licensed Artist AU to be discoverable to a standalone
# host process while Vibe renders and all measurement remain automatic.

set -euo pipefail

if (( $# != 4 )); then
  print -u2 "usage: $0 <raw-input.wav> <artist-reference.wav> <output-directory> <retune-ms>"
  exit 2
fi

raw=$1
artist=$2
output=$3
retune=$4
root_dir=${0:A:h:h}
vibe_host="$root_dir/build-tail-test/VibeAutotuneSignalChainDiagnostic"
compare_tool="$root_dir/tools/measure_artist_vibe_latency_aligned.py"

[[ -f "$raw" && -f "$artist" && -x "$vibe_host" && -f "$compare_tool" ]] || {
  print -u2 "missing input/reference or required diagnostic tool"
  exit 2
}

mkdir -p "$output/vibe" "$output/comparison"
VIBE_DIAGNOSTIC_BLOCK_SIZE=32 \
VIBE_RAPID_ADJACENT_SELECTOR=1 \
VIBE_RETIMABLE_EPOCHS=1 \
  "$vibe_host" "$raw" "$output/vibe" 0 1 "$retune" 0 0 0 0 > "$output/vibe.log" 2>&1
python3 "$compare_tool" "$raw" "$artist" "$output/vibe/full_default_render.wav" "$output/comparison" > "$output/comparison.log" 2>&1
print "comparison complete: $output/comparison/latency_aligned_metrics.json"
