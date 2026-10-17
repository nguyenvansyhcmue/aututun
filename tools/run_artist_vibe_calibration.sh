#!/bin/zsh
# Batch calibration runner for the locally installed Auto-Tune Artist AU and
# VibeAutotune's production TD-PSOLA diagnostic path.  It never changes the
# plug-in or its defaults: every render is written below the requested output
# directory for offline comparison.

set -euo pipefail

if (( $# < 2 || $# > 3 )); then
  print -u2 "usage: $0 <input-wav-or-directory> <output-directory> [retune-ms-list]"
  print -u2 "example: $0 '$HOME/Downloads/calibration-input' artifacts/artist-vs-vibe-auto '0 20 50 100'"
  exit 2
fi

input_path=$1
output_root=$2
retune_values=${3:-"0 20 50 100"}
root_dir=${0:A:h:h}
build_dir="$root_dir/build-tail-test"
artist_host="$build_dir/VibeAutotuneArtistNativeAURender"
vibe_host="$build_dir/VibeAutotuneSignalChainDiagnostic"
compare_tool="$root_dir/tools/compare_pitch_correction_bounces.py"

for required in "$artist_host" "$vibe_host" "$compare_tool"; do
  if [[ ! -e "$required" ]]; then
    print -u2 "missing required tool: $required"
    print -u2 "Build the calibration targets first: cmake --build build-tail-test --target VibeAutotuneArtistNativeAURender VibeAutotuneSignalChainDiagnostic"
    exit 2
  fi
done

mkdir -p "$output_root"
index_file="$output_root/index.csv"
print 'fixture,retune_ms,artist_wav,vibe_wav,metrics_json,status' > "$index_file"

typeset -a fixtures
if [[ -d "$input_path" ]]; then
  fixtures=("$input_path"/**/*.wav(N))
else
  fixtures=("$input_path")
fi

if (( ${#fixtures} == 0 )); then
  print -u2 "no WAV fixtures found: $input_path"
  exit 2
fi

for fixture in "${fixtures[@]}"; do
  if [[ ! -f "$fixture" ]]; then
    print -u2 "not a file: $fixture"
    exit 2
  fi
  fixture_name=${${fixture:t}:r}
  for retune in ${(z)retune_values}; do
    case_dir="$output_root/${fixture_name}/retune-${retune}ms"
    artist_wav="$case_dir/artist.wav"
    vibe_dir="$case_dir/vibe"
    metrics_dir="$case_dir/comparison"
    mkdir -p "$case_dir"

    print "[calibrate] fixture=$fixture_name retune=${retune}ms"
    "$artist_host" "$fixture" "$artist_wav" "$retune" > "$case_dir/artist.log" 2>&1

    # Explicit trailing zero selects the production TD-PSOLA renderer rather
    # than the retained phase-continuous diagnostic path. The environment
    # matches the live 32-sample scheduler and production retiming.
    VIBE_DIAGNOSTIC_BLOCK_SIZE=32 \
    VIBE_RAPID_ADJACENT_SELECTOR=1 \
    VIBE_RETIMABLE_EPOCHS=1 \
      "$vibe_host" "$fixture" "$vibe_dir" 0 1 "$retune" 0 0 0 0 > "$case_dir/vibe.log" 2>&1

    python3 "$compare_tool" "$artist_wav" "$vibe_dir/full_default_render.wav" "$metrics_dir" \
      > "$case_dir/comparison.log" 2>&1
    print -r -- "\"$fixture\",$retune,\"$artist_wav\",\"$vibe_dir/full_default_render.wav\",\"$metrics_dir/metrics.json\",PASS" >> "$index_file"
  done
done

print "[calibrate] complete: $index_file"
