#pragma once

#include "eye/eye_types.h"

#include <vector>

namespace bro::eye {

/// Produces diagnostic images (audit overlays, diff heatmaps, exposure zones,
/// FFT tiling spectra, depth/normals views, or labelled contact sheets) with
/// readable bitmap labels, colorbars, and optional PNG export.
EyeVisualResult visualizeImages(const std::vector<EyeImageView>& images,
                                const EyeVisualizeOptions& opts = {});

}  // namespace bro::eye
