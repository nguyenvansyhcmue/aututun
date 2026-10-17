#!/bin/zsh
# Render VibeAutotune for every verified Artist-control row, then write a
# reproducible waveform/spectral comparison beside each render.
#
# Usage:
#   scripts/measure_artist_control_manifest.zsh [output-directory] [id-regex]
#
# The manifest, not legacy WAV filenames, owns the parameter mapping.
set -euo pipefail

repo_root=${0:A:h:h}
cd "$repo_root"

manifest="benchmark/autotune-artist-controls/TARGET_MANIFEST.csv"
input_provenance="benchmark/autotune-artist-controls/BENCHMARK_INPUT_PROVENANCE.csv"
output_root=${1:-"renders/control-manifest-measurement"}
id_pattern=${2:-".*"}
# Keep this benchmark tied to the executable being evaluated rather than to
# an arbitrary build directory.  The current production path reports a fixed
# 1152-sample renderer latency; callers may override it for an explicitly
# different build/configuration, but it must never be guessed per target.
build_directory=${VIBE_BUILD_DIRECTORY:-build-tail-test}
latency_samples=${VIBE_MATCH_LATENCY_SAMPLES:-1152}
renderer="$build_directory/VibeAutotuneSignalChainDiagnostic"
matcher="$build_directory/VibeAutotuneTargetReferenceMatch"
pitch_exporter="$build_directory/VibeAutotuneIndependentPitchContour"
pitch_matcher="scripts/measure_control_pitch_match.py"

[[ -x "$renderer" && -x "$matcher" && -x "$pitch_exporter" && -f "$pitch_matcher" ]] || {
  print -u2 "Build SignalChainDiagnostic, TargetReferenceMatch and IndependentPitchContour first."
  exit 2
}
[[ -f "$input_provenance" ]] || {
  print -u2 "Missing $input_provenance. Refusing to benchmark an unproven raw input."
  exit 2
}

typeset -A input_status input_sha
while IFS=, read -r provenance_input provenance_status provenance_sha provenance_notes; do
  [[ "$provenance_input" == \#* || "$provenance_input" == "input_timeline" ]] && continue
  input_status[$provenance_input]="$provenance_status"
  input_sha[$provenance_input]="$provenance_sha"
done < "$input_provenance"

mkdir -p "$output_root"
lock_directory="$output_root/.measurement-lock"
if ! mkdir "$lock_directory" 2>/dev/null; then
  print -u2 "A manifest measurement is already writing $output_root. Wait for it to finish, or inspect $lock_directory before retrying."
  exit 3
fi
trap 'rmdir "$lock_directory" 2>/dev/null || true' EXIT INT TERM

summary="$output_root/summary.csv"
skipped="$output_root/skipped.csv"
print 'id,control_family,retune_ms,flex_tune,humanize_ms,natural_vibrato_ms,input_status,waveform_nrmse,envelope_rmse_db,spectral_rmse_db,spectral_tilt_rmse_db,target_max_step,candidate_max_step,f0_rmse_cents,f0_matched_frames' > "$summary"
print 'id,reason' > "$skipped"

# Fields are safe to split on commas: all paths and notes in the manifest are
# deliberately comma-free. Every verified row is an exact four-control tuple;
# Retune anchors must pass through the same pipeline as the other families.
awk -F, -v pattern="$id_pattern" \
  '$1 !~ /^#/ && $1 != "id" && $16 == "verified" && $1 ~ pattern { print }' "$manifest" |
while IFS=, read -r id family target_rel input_rel retune flex humanize vibrato tracking key scale classic formant duration checksum row_status notes; do
  if [[ -f "$target_rel" ]]; then
    target="$target_rel"
  else
    target="benchmark/autotune-artist-controls/$target_rel"
  fi
  if [[ -f "$input_rel" ]]; then
    input="$input_rel"
  else
    input="benchmark/autotune-artist-controls/$input_rel"
  fi
  [[ -f "$target" && -f "$input" ]] || {
    print -u2 "Missing target or input for $id: target=$target input=$input"
    exit 4
  }
  [[ -n "${input_status[$input_rel]-}" && -n "${input_sha[$input_rel]-}" ]] || {
    print -u2 "No raw-input provenance record for $id ($input_rel)."
    exit 5
  }
  actual_target_sha=$(shasum -a 256 "$target" | awk '{ print $1 }')
  [[ "$actual_target_sha" == "$checksum" ]] || {
    print -u2 "Target SHA-256 mismatch for $id: expected $checksum, got $actual_target_sha"
    exit 6
  }
  actual_input_sha=$(shasum -a 256 "$input" | awk '{ print $1 }')
  [[ "$actual_input_sha" == "${input_sha[$input_rel]}" ]] || {
    print -u2 "Raw-input SHA-256 mismatch for $id: expected ${input_sha[$input_rel]}, got $actual_input_sha"
    exit 7
  }
  if [[ "${input_status[$input_rel]}" != "verified" && "${VIBE_ALLOW_PROVISIONAL_INPUTS:-0}" != "1" ]]; then
    print "$id,provisional raw input ($input_rel); strict benchmark skipped" >> "$skipped"
    print "skipped $id (provisional raw input)"
    continue
  fi
  case_dir="$output_root/$id"
  mkdir -p "$case_dir"

  if [[ ! -f "$case_dir/reference_metrics.txt" ]]; then
    # Production uses the TD-PSOLA renderer (`usePhaseContinuousRenderer` is
    # false in prepareToPlay). Pass 0 explicitly so this benchmark cannot
    # silently follow the diagnostic executable's alternative default.
    "$renderer" "$input" "$case_dir" \
      0 0 "$retune" "$flex" 0 0 0 0 0 "$humanize" "$vibrato" \
      > "$case_dir/diagnostic.log"
    "$matcher" "$target" "$case_dir/full_default_render.wav" "$latency_samples" \
      > "$case_dir/reference_metrics.txt"
    print "rendered $id"
  fi

  # Older runs used an exact-sample contour join and therefore wrote a NaN
  # metric whenever latency was not divisible by the 256-sample contour hop.
  # Recompute those cached rows with the fixed-latency interpolation metric.
  if [[ ! -f "$case_dir/pitch_metrics.txt" || "$(<"$case_dir/pitch_metrics.txt")" == *"f0_rmse_cents=nan"* ]]; then
    # Synthetic control timelines sit around F#4. The full vocal Retune
    # references span the Alto detector range and must not be truncated to
    # the synthetic 300--500 Hz window.
    if [[ "$family" == "retune" ]]; then
      minimum_f0=94
      maximum_f0=600
    else
      minimum_f0=300
      maximum_f0=500
    fi
    "$pitch_exporter" "$target" "$minimum_f0" "$maximum_f0" > "$case_dir/target_contour.csv"
    "$pitch_exporter" "$case_dir/full_default_render.wav" "$minimum_f0" "$maximum_f0" > "$case_dir/candidate_contour.csv"
    python3 "$pitch_matcher" "$case_dir/target_contour.csv" "$case_dir/candidate_contour.csv" \
      "$latency_samples" --frames "$case_dir/pitch_error_frames.csv" > "$case_dir/pitch_metrics.txt"
  fi

  metrics=$(awk '
    /waveform_nrmse=/ {
      for (i = 1; i <= NF; ++i) {
        split($i, pair, "=")
        values[pair[1]] = pair[2]
      }
    }
    END {
      printf "%s,%s,%s,%s,%s,%s", values["waveform_nrmse"], values["envelope_rmse_db"], values["spectral_rmse_db"], values["spectral_tilt_rmse_db"], values["target_max_step"], values["candidate_max_step"]
    }' "$case_dir/reference_metrics.txt")
  pitch=$(awk '
    /f0_rmse_cents=/ {
      for (i = 1; i <= NF; ++i) {
        split($i, pair, "=")
        values[pair[1]] = pair[2]
      }
    }
    END { printf "%s,%s", values["f0_rmse_cents"], values["f0_matched_frames"] }' "$case_dir/pitch_metrics.txt")
  print "$id,$family,$retune,$flex,$humanize,$vibrato,${input_status[$input_rel]},$metrics,$pitch" >> "$summary"
  print "indexed $id"
done

print "Wrote $summary"
