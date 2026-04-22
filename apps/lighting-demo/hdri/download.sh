#!/usr/bin/env bash
# Fetch the demo's HDR environment maps from Poly Haven (CC0 / public domain).
# All 12 are free to redistribute, so this script is the install path —
# the .hdr files are .gitignored to keep the repo small.
#
#   ./download.sh            # default: 2k (~6 MB each, ~70 MB total)
#   RES=4k ./download.sh     # crisper skybox  (~23 MB each, ~270 MB total)
#   RES=1k ./download.sh     # quick start     (~1.5 MB each, ~17 MB total)
#
# Re-running is a no-op for files already present at the chosen resolution.

set -euo pipefail
RES="${RES:-2k}"
case "$RES" in 1k|2k|4k|8k) ;; *) echo "RES must be 1k/2k/4k/8k (got: $RES)" >&2; exit 2;; esac

cd "$(dirname "$0")"

HDRIS=(
  venice_sunset
  kloppenheim_06_puresky
  spruit_sunrise
  kiara_1_dawn
  dikhololo_night
  qwantani_puresky
  the_sky_is_on_fire
  moonless_golf
  kloofendal_43d_clear_puresky
  spiaggia_di_mondello
  snowy_forest_path_01
  belfast_sunset_puresky
)

echo "Downloading ${#HDRIS[@]} HDRIs at $RES into $(pwd)"
for slug in "${HDRIS[@]}"; do
  out="${slug}_${RES}.hdr"
  if [ -f "$out" ]; then
    echo "  [skip] $out"
    continue
  fi
  url="https://dl.polyhaven.org/file/ph-assets/HDRIs/hdr/${RES}/${slug}_${RES}.hdr"
  echo "  [get ] $slug"
  if curl -fsSL -o "$out.tmp" "$url"; then
    mv "$out.tmp" "$out"
  else
    echo "  [FAIL] $slug" >&2
    rm -f "$out.tmp"
  fi
done

echo
ls -lh *.hdr 2>/dev/null || echo "(none — all downloads failed?)"
