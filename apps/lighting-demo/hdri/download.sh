#!/usr/bin/env bash
# Public-domain (CC0) HDRIs from Poly Haven, 1k resolution (~3-6MB each).
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
for slug in "${HDRIS[@]}"; do
  url="https://dl.polyhaven.org/file/ph-assets/HDRIs/hdr/1k/${slug}_1k.hdr"
  out="${slug}_1k.hdr"
  if [ -f "$out" ]; then
    echo "[skip] $out"
    continue
  fi
  echo "[get ] $slug"
  curl -fsSL -o "$out.tmp" "$url" && mv "$out.tmp" "$out" || { echo "[FAIL] $slug"; rm -f "$out.tmp"; }
done
ls -lh
